/* Audio relay, client side. Swapp adds an output device, "Swapp Audio Relay"
 * (a null sink named swapp_relay), records what is played into it from its
 * monitor, and while the TCP link is up sends it to the server as one Opus
 * datagram per 10 ms frame.
 *
 * Everything Pulse-related runs on a pa_threaded_mainloop: the callbacks
 * there set up the sink and the stream, and the read callback encodes and
 * sends each frame itself, so there is no thread of our own. The capture
 * state below (frame buffer, encoder, socket) is touched only by those
 * callbacks, which run with the mainloop lock held, or by the GTK thread with
 * that lock held or the mainloop stopped. g_lock guards only the status the
 * tray reads.
 *
 * Losing the Pulse server (a PipeWire restart, or none running at all) is
 * retried from a 2 s GLib timeout on the GTK thread rather than from inside
 * the mainloop: a dead context has to be torn down with its whole mainloop,
 * and a pa_threaded_mainloop cannot stop itself from its own thread. */

#include "audio.h"
#include "audio_packet.h"
#include "net.h"

#include <arpa/inet.h>
#include <glib.h>
#include <netinet/in.h>
#include <opus.h>
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SINK_NAME "swapp_relay"
/* The quoting is what PipeWire 1.4's pipewire-pulse needs for a description
 * with spaces; without the inner quotes the device is just called "Swapp". */
#define SINK_ARGS                                                                          \
    "sink_name=" SINK_NAME " sink_properties='device.description=\"Swapp Audio Relay\"' " \
    "rate=48000 channels=2"
#define RETRY_MS 2000
#define UNLOAD_WAIT_US (300 * 1000)

typedef enum { STATUS_UNAVAILABLE, STATUS_WAITING, STATUS_SENDING } swapp_audio_status;

static GMutex g_lock;
static int g_sending = 0;
static char g_text[96] = "Audio: waiting for the server";
static void (*g_state_callback)(void) = NULL;

/* Owned by the GTK thread. */
static int g_started = 0;
static guint g_retry_source = 0;
static pa_threaded_mainloop *g_ml = NULL;
static pa_context *g_ctx = NULL;
static OpusEncoder *g_enc = NULL;

/* Set from the mainloop thread when the context, the module or the stream
 * fails; the retry timeout rebuilds everything when it sees it. */
static gint g_failed = 0;

/* Capture state; see the top of the file for who may touch it. */
static pa_stream *g_stream = NULL;
static int g_adopting = 0;
static uint32_t g_module = PA_INVALID_INDEX;
static uint32_t g_sink = PA_INVALID_INDEX;
static float g_frame[SWAPP_AUDIO_FRAME * SWAPP_AUDIO_CHANNELS];
static int g_frame_fill = 0;
static int g_fd = -1;
static uint32_t g_cur_peer = 0, g_cur_self = 0;
static uint32_t g_stream_id = 0;
static uint32_t g_seq = 0;

static gboolean swapp_audio_notify(gpointer data) {
    (void)data;
    if (g_state_callback) {
        g_state_callback();
    }
    return G_SOURCE_REMOVE;
}

/* Updates the status and hops to the GTK thread, only when the text changed. */
static void swapp_audio_set_status(swapp_audio_status status, uint32_t peer_be) {
    char text[96];
    if (status == STATUS_SENDING) {
        char ip[INET_ADDRSTRLEN];
        struct in_addr addr = {.s_addr = peer_be};
        inet_ntop(AF_INET, &addr, ip, sizeof ip);
        snprintf(text, sizeof text, "Audio: sending to %s", ip);
    } else if (status == STATUS_WAITING) {
        snprintf(text, sizeof text, "Audio: waiting for the server");
    } else {
        snprintf(text, sizeof text, "Audio: relay device unavailable");
    }
    g_mutex_lock(&g_lock);
    int changed = strcmp(text, g_text) != 0;
    if (changed) {
        memcpy(g_text, text, sizeof text);
    }
    g_sending = status == STATUS_SENDING;
    g_mutex_unlock(&g_lock);
    if (changed) {
        g_idle_add(swapp_audio_notify, NULL);
    }
}

static void swapp_audio_fail(void) {
    g_atomic_int_set(&g_failed, 1);
    swapp_audio_set_status(STATUS_UNAVAILABLE, 0);
}

/* Binds to the LAN address the TCP link uses, for the same reason the link's
 * own sockets are: so VPN routes cannot pull the datagrams onto tun0. */
static int swapp_audio_open(uint32_t self_be, uint32_t peer_be) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_addr.s_addr = self_be};
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(SWAPP_NET_PORT),
                                 .sin_addr.s_addr = peer_be};
    if (bind(fd, (struct sockaddr *)&local, sizeof local) != 0
        || connect(fd, (struct sockaddr *)&remote, sizeof remote) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void swapp_audio_close_socket(void) {
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

/* One full frame: encode and send it if the server is there, otherwise drop
 * it unencoded. */
static void swapp_audio_send_frame(void) {
    uint32_t peer_be, self_be;
    if (!swapp_net_peer(&peer_be, &self_be)) {
        swapp_audio_close_socket();
        swapp_audio_set_status(STATUS_WAITING, 0);
        return;
    }
    if (g_fd >= 0 && (peer_be != g_cur_peer || self_be != g_cur_self)) {
        swapp_audio_close_socket();
    }
    if (g_fd < 0) {
        g_fd = swapp_audio_open(self_be, peer_be);
        g_cur_peer = peer_be;
        g_cur_self = self_be;
        if (g_fd < 0) {
            swapp_audio_set_status(STATUS_WAITING, 0);
            return;
        }
        /* A new stream id per socket tells the server to reset its decoder,
         * so the encoder starts over with it. */
        g_stream_id = g_random_int();
        g_seq = 0;
        opus_encoder_ctl(g_enc, OPUS_RESET_STATE);
    }

    uint8_t opus[SWAPP_AUDIO_MAX_PAYLOAD];
    opus_int32 len = opus_encode_float(g_enc, g_frame, SWAPP_AUDIO_FRAME, opus, sizeof opus);
    if (len < 0) {
        return;
    }
    uint8_t buf[SWAPP_AUDIO_HEADER_SIZE + SWAPP_AUDIO_MAX_PAYLOAD];
    size_t n = swapp_audio_packet_write(buf, sizeof buf, g_stream_id, g_seq++, opus, (size_t)len);
    /* Never block the mainloop on a full socket buffer: a dropped frame is
     * concealed on the server. An ICMP refusal from an unlistening server
     * surfaces as an error on a later send and is equally harmless. */
    (void)send(g_fd, buf, n, MSG_DONTWAIT);
    swapp_audio_set_status(STATUS_SENDING, peer_be);
}

static void swapp_audio_on_read(pa_stream *s, size_t nbytes, void *userdata) {
    (void)nbytes;
    (void)userdata;
    for (;;) {
        const void *data;
        size_t len;
        if (pa_stream_peek(s, &data, &len) < 0 || len == 0) {
            return;
        }
        /* data is NULL for a hole in the buffer: nothing to capture there.
         * No gain is applied: the monitor already carries the sink's volume
         * and mute, because pipewire-pulse creates the null sink with
         * monitor.channel-volumes set. */
        if (data) {
            const uint8_t *bytes = data;
            size_t frame_bytes = sizeof(float) * SWAPP_AUDIO_CHANNELS;
            size_t frames = len / frame_bytes;
            while (frames > 0) {
                size_t take = (size_t)(SWAPP_AUDIO_FRAME - g_frame_fill);
                if (take > frames) {
                    take = frames;
                }
                memcpy(g_frame + g_frame_fill * SWAPP_AUDIO_CHANNELS, bytes, take * frame_bytes);
                bytes += take * frame_bytes;
                frames -= take;
                g_frame_fill += (int)take;
                if (g_frame_fill == SWAPP_AUDIO_FRAME) {
                    swapp_audio_send_frame();
                    g_frame_fill = 0;
                }
            }
        }
        pa_stream_drop(s);
    }
}

static void swapp_audio_on_stream_state(pa_stream *s, void *userdata) {
    (void)userdata;
    switch (pa_stream_get_state(s)) {
    case PA_STREAM_READY:
        swapp_audio_set_status(STATUS_WAITING, 0);
        break;
    case PA_STREAM_FAILED:
    case PA_STREAM_TERMINATED:
        /* Most likely the device was removed under us; the retry recreates it. */
        swapp_audio_fail();
        break;
    default:
        break;
    }
}

static void swapp_audio_start_stream(pa_context *c) {
    static const pa_sample_spec spec = {
        .format = PA_SAMPLE_FLOAT32LE, .rate = SWAPP_AUDIO_RATE, .channels = SWAPP_AUDIO_CHANNELS};
    pa_channel_map map;
    pa_channel_map_init_stereo(&map);
    g_stream = pa_stream_new(c, "Swapp Audio Relay", &spec, &map);
    if (!g_stream) {
        swapp_audio_fail();
        return;
    }
    pa_stream_set_state_callback(g_stream, swapp_audio_on_stream_state, NULL);
    pa_stream_set_read_callback(g_stream, swapp_audio_on_read, NULL);
    pa_buffer_attr attr = {.maxlength = (uint32_t)-1,
                           .tlength = (uint32_t)-1,
                           .prebuf = (uint32_t)-1,
                           .minreq = (uint32_t)-1,
                           .fragsize = SWAPP_AUDIO_FRAME * SWAPP_AUDIO_CHANNELS * sizeof(float)};
    g_frame_fill = 0;
    if (pa_stream_connect_record(g_stream, SINK_NAME ".monitor", &attr,
                                 PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE)
        < 0) {
        swapp_audio_fail();
    }
}

static void swapp_audio_on_sink(pa_context *c, const pa_sink_info *info, int eol, void *userdata);

static void swapp_audio_on_module(pa_context *c, uint32_t index, void *userdata) {
    (void)userdata;
    if (index == PA_INVALID_INDEX) {
        swapp_audio_fail();
        return;
    }
    g_module = index;
    g_adopting = 0;
    pa_operation *op = pa_context_get_sink_info_by_name(c, SINK_NAME, swapp_audio_on_sink, NULL);
    if (op) {
        pa_operation_unref(op);
    }
}

/* The setup lookup: adopt the device if it is already there (a previous copy
 * left it across an update), else create it and look again. */
static void swapp_audio_on_sink(pa_context *c, const pa_sink_info *info, int eol, void *userdata) {
    (void)userdata;
    if (info) {
        g_sink = info->index;
        if (g_adopting) {
            g_module = info->owner_module;
        }
        if (!g_stream) {
            swapp_audio_start_stream(c);
        }
        return;
    }
    if (!eol || g_sink != PA_INVALID_INDEX) {
        return;
    }
    if (!g_adopting) {
        /* Loaded, yet not found by name. */
        swapp_audio_fail();
        return;
    }
    pa_operation *op = pa_context_load_module(c, "module-null-sink", SINK_ARGS, swapp_audio_on_module, NULL);
    if (op) {
        pa_operation_unref(op);
    } else {
        swapp_audio_fail();
    }
}

static void swapp_audio_on_event(pa_context *c, pa_subscription_event_type_t type, uint32_t index,
                                 void *userdata) {
    (void)c;
    (void)userdata;
    /* Someone unloaded the device. pipewire-pulse leaves the capture stream
     * connected but silent rather than failing it, so this is the only sign;
     * the retry recreates the device. */
    if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK
        && (type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE
        && index == g_sink) {
        swapp_audio_fail();
    }
}

static void swapp_audio_on_context_state(pa_context *c, void *userdata) {
    (void)userdata;
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY: {
        pa_operation *op = pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK, NULL, NULL);
        if (op) {
            pa_operation_unref(op);
        }
        g_adopting = 1;
        op = pa_context_get_sink_info_by_name(c, SINK_NAME, swapp_audio_on_sink, NULL);
        if (op) {
            pa_operation_unref(op);
        } else {
            swapp_audio_fail();
        }
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        swapp_audio_fail();
        break;
    default:
        break;
    }
}

/* Creates the mainloop and connects; the rest happens in the callbacks. */
static void swapp_audio_connect(void) {
    g_atomic_int_set(&g_failed, 0);
    g_module = PA_INVALID_INDEX;
    g_sink = PA_INVALID_INDEX;
    g_ml = pa_threaded_mainloop_new();
    if (!g_ml) {
        swapp_audio_fail();
        return;
    }
    g_ctx = pa_context_new(pa_threaded_mainloop_get_api(g_ml), "Swapp");
    if (!g_ctx) {
        swapp_audio_fail();
        return;
    }
    pa_context_set_state_callback(g_ctx, swapp_audio_on_context_state, NULL);
    pa_context_set_subscribe_callback(g_ctx, swapp_audio_on_event, NULL);
    /* No autospawn: with no Pulse server the device is simply unavailable
     * until one appears, and the retry picks it up. */
    if (pa_context_connect(g_ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0
        || pa_threaded_mainloop_start(g_ml) < 0) {
        swapp_audio_fail();
    }
}

/* Waits for op without pa_threaded_mainloop_wait, which has no timeout. Called
 * and returns with the mainloop lock held. */
static void swapp_audio_wait_op(pa_operation *op, gint64 deadline) {
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING && g_get_monotonic_time() < deadline) {
        pa_threaded_mainloop_unlock(g_ml);
        g_usleep(5 * 1000);
        pa_threaded_mainloop_lock(g_ml);
    }
    if (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_operation_cancel(op);
    }
    pa_operation_unref(op);
}

/* Tears down the stream, the context and the mainloop, removing the device
 * first if asked. Only the unload waits on the server, and for at most
 * UNLOAD_WAIT_US; everything else is local. */
static void swapp_audio_disconnect(int remove_device) {
    if (!g_ml) {
        return;
    }
    pa_threaded_mainloop_lock(g_ml);
    if (g_stream) {
        pa_stream_set_state_callback(g_stream, NULL, NULL);
        pa_stream_set_read_callback(g_stream, NULL, NULL);
        pa_stream_disconnect(g_stream);
        pa_stream_unref(g_stream);
        g_stream = NULL;
    }
    if (g_ctx) {
        pa_context_set_state_callback(g_ctx, NULL, NULL);
        pa_context_set_subscribe_callback(g_ctx, NULL, NULL);
        if (remove_device && g_module != PA_INVALID_INDEX
            && pa_context_get_state(g_ctx) == PA_CONTEXT_READY) {
            pa_operation *op = pa_context_unload_module(g_ctx, g_module, NULL, NULL);
            if (op) {
                swapp_audio_wait_op(op, g_get_monotonic_time() + UNLOAD_WAIT_US);
            }
        }
        pa_context_disconnect(g_ctx);
        pa_context_unref(g_ctx);
        g_ctx = NULL;
    }
    pa_threaded_mainloop_unlock(g_ml);
    pa_threaded_mainloop_stop(g_ml);
    pa_threaded_mainloop_free(g_ml);
    g_ml = NULL;
    swapp_audio_close_socket();
}

static gboolean swapp_audio_retry(gpointer data) {
    (void)data;
    if (g_atomic_int_get(&g_failed)) {
        swapp_audio_disconnect(0);
        swapp_audio_connect();
    }
    return G_SOURCE_CONTINUE;
}

void swapp_audio_start(void) {
    if (g_started) {
        return;
    }
    g_started = 1;
    int err;
    g_enc = opus_encoder_create(SWAPP_AUDIO_RATE, SWAPP_AUDIO_CHANNELS, OPUS_APPLICATION_AUDIO, &err);
    if (!g_enc) {
        swapp_audio_set_status(STATUS_UNAVAILABLE, 0);
        return;
    }
    opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(SWAPP_AUDIO_BITRATE));
    swapp_audio_connect();
    g_retry_source = g_timeout_add(RETRY_MS, swapp_audio_retry, NULL);
}

/* Bounded by construction rather than by a deadline: the only wait on the
 * server is the unload (at most UNLOAD_WAIT_US), and the mainloop thread
 * never blocks (sends are MSG_DONTWAIT), so stopping it is prompt. The whole
 * stop stays inside the 500 ms audio.h promises. */
void swapp_audio_stop(int remove_device) {
    if (!g_started) {
        return;
    }
    g_started = 0;
    if (g_retry_source) {
        g_source_remove(g_retry_source);
        g_retry_source = 0;
    }
    swapp_audio_disconnect(remove_device);
    if (g_enc) {
        opus_encoder_destroy(g_enc);
        g_enc = NULL;
    }
}

int swapp_audio_ok(void) {
    g_mutex_lock(&g_lock);
    int ok = g_sending;
    g_mutex_unlock(&g_lock);
    return ok;
}

void swapp_audio_status_text(char *buf, size_t buf_size) {
    g_mutex_lock(&g_lock);
    snprintf(buf, buf_size, "%s", g_text);
    g_mutex_unlock(&g_lock);
}

void swapp_audio_set_state_callback(void (*callback)(void)) {
    g_state_callback = callback;
}

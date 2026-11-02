/* Audio relay, client side. For now: while the TCP link is up, send an empty
 * probe datagram to the server once a second so the server can tell the UDP
 * path works. Real audio replaces this thread's loop later. */

#include "audio.h"
#include "audio_packet.h"
#include "net.h"

#include <arpa/inet.h>
#include <glib.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PROBE_INTERVAL_US (1000 * 1000)
#define STOP_WAIT_US (500 * 1000)

static GMutex g_lock;
static GCond g_cond;
static GThread *g_thread = NULL;
static int g_stop = 0;
static int g_finished = 0;
static int g_sending = 0;
static uint32_t g_peer_be = 0;
static char g_text[96] = "Audio: waiting for the server";
static void (*g_state_callback)(void) = NULL;

static gboolean swapp_audio_notify(gpointer data) {
    (void)data;
    if (g_state_callback) {
        g_state_callback();
    }
    return G_SOURCE_REMOVE;
}

/* Updates the status and hops to the GTK thread, only when the text changed. */
static void swapp_audio_set_status(int sending, uint32_t peer_be) {
    char text[96];
    if (sending) {
        char ip[INET_ADDRSTRLEN];
        struct in_addr addr = {.s_addr = peer_be};
        inet_ntop(AF_INET, &addr, ip, sizeof ip);
        snprintf(text, sizeof text, "Audio: sending to %s", ip);
    } else {
        snprintf(text, sizeof text, "Audio: waiting for the server");
    }
    g_mutex_lock(&g_lock);
    int changed = strcmp(text, g_text) != 0;
    if (changed) {
        memcpy(g_text, text, sizeof text);
    }
    g_sending = sending;
    g_mutex_unlock(&g_lock);
    if (changed) {
        g_idle_add(swapp_audio_notify, NULL);
    }
}

static int swapp_audio_stopping(void) {
    g_mutex_lock(&g_lock);
    int stop = g_stop;
    g_mutex_unlock(&g_lock);
    return stop;
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

static gpointer swapp_audio_thread(gpointer data) {
    (void)data;
    uint32_t stream_id = g_random_int();
    uint32_t seq = 0;
    int fd = -1;
    uint32_t cur_peer = 0, cur_self = 0;

    while (!swapp_audio_stopping()) {
        uint32_t peer_be, self_be;
        if (swapp_net_peer(&peer_be, &self_be)) {
            if (fd >= 0 && (peer_be != cur_peer || self_be != cur_self)) {
                close(fd);
                fd = -1;
            }
            if (fd < 0) {
                fd = swapp_audio_open(self_be, peer_be);
                cur_peer = peer_be;
                cur_self = self_be;
            }
            if (fd >= 0) {
                uint8_t buf[SWAPP_AUDIO_HEADER_SIZE];
                size_t n = swapp_audio_packet_write(buf, sizeof buf, stream_id, seq++, NULL, 0);
                /* An ICMP refusal from an unlistening server surfaces as an
                 * error on a later send; the next probe simply tries again. */
                (void)send(fd, buf, n, 0);
            }
            swapp_audio_set_status(fd >= 0, peer_be);
        } else {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
            swapp_audio_set_status(0, 0);
        }

        /* Sleep on the condition rather than g_usleep so stop is prompt. */
        g_mutex_lock(&g_lock);
        gint64 deadline = g_get_monotonic_time() + PROBE_INTERVAL_US;
        while (!g_stop && g_cond_wait_until(&g_cond, &g_lock, deadline)) {
        }
        g_mutex_unlock(&g_lock);
    }

    if (fd >= 0) {
        close(fd);
    }
    g_mutex_lock(&g_lock);
    g_finished = 1;
    g_cond_broadcast(&g_cond);
    g_mutex_unlock(&g_lock);
    return NULL;
}

void swapp_audio_start(void) {
    g_mutex_lock(&g_lock);
    if (g_thread) {
        g_mutex_unlock(&g_lock);
        return;
    }
    g_stop = 0;
    g_finished = 0;
    g_mutex_unlock(&g_lock);
    g_thread = g_thread_new("swapp-audio", swapp_audio_thread, NULL);
}

void swapp_audio_stop(int remove_device) {
    /* Nothing to remove until the client creates a virtual device. */
    (void)remove_device;
    if (!g_thread) {
        return;
    }
    g_mutex_lock(&g_lock);
    g_stop = 1;
    g_cond_broadcast(&g_cond);
    /* g_thread_join has no timeout, so wait for the thread's own exit signal
     * and join only if it came; otherwise let go of the thread and move on. */
    gint64 deadline = g_get_monotonic_time() + STOP_WAIT_US;
    while (!g_finished && g_cond_wait_until(&g_cond, &g_lock, deadline)) {
    }
    int finished = g_finished;
    g_mutex_unlock(&g_lock);
    if (finished) {
        g_thread_join(g_thread);
    } else {
        g_thread_unref(g_thread);
    }
    g_thread = NULL;
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

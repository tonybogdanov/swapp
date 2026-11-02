/* Audio relay, server side: receive the client's Opus datagrams over UDP,
 * reorder them in a jitter buffer, decode them and play them on the default
 * output device through WASAPI shared mode, following the default when it
 * changes.
 *
 * Two threads. The receiver owns the socket and only fills the jitter buffer;
 * the render thread owns COM, the decoder and the device, and drains it at
 * the device's pace. The jitter buffer and the status are all they share. */

#define COBJMACROS

#include "audio.h"
#include "audio_jitter.h"
#include "audio_packet.h"
#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <avrt.h>

#include <opus.h>

#define STOP_WAIT_MS 500
#define RETRY_MS 1000        /* no output device: how often to look again */
#define REOPEN_PAUSE_MS 100  /* after a stream failure, so a broken device cannot spin us */
#define RENDER_WAIT_MS 1000  /* a device that stops signalling is noticed by polling it */
#define BUFFER_HNS 200000    /* 20 ms, in 100 ns units */
#ifndef AUDCLNT_STREAMFLAGS_RATEADJUST
#define AUDCLNT_STREAMFLAGS_RATEADJUST 0x00100000
#endif
#define DRIFT_STEP_FRAMES 100 /* 1 s of 10 ms frames: how often the rate and the readout move */
#define TARGET_FRAMES (SWAPP_AUDIO_TARGET_MS * SWAPP_AUDIO_RATE / 1000 / SWAPP_AUDIO_FRAME)

/* The Windows SDK headers only declare these MIDL-generated IDs, which
 * initguid.h does not reach, so whether they link under MSVC would depend on
 * its import libraries; spelled out here, they always do. initguid.h is still
 * needed: it is what defines PKEY_Device_FriendlyName. */
static const GUID swapp_clsid_enumerator = {
    0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID swapp_iid_enumerator = {
    0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const GUID swapp_iid_notification = {
    0x7991eec9, 0x7e89, 0x4d85, {0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}};
static const GUID swapp_iid_audio_client = {
    0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const GUID swapp_iid_clock_adjustment = {
    0xf6e4c0a0, 0x46d9, 0x4fb8, {0xbe, 0x21, 0x57, 0xa3, 0xef, 0x2b, 0x62, 0x6c}};
static const GUID swapp_iid_render_client = {
    0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
static const GUID swapp_iid_unknown = {
    0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID swapp_subtype_float = { /* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT */
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

typedef enum { OUTPUT_PENDING, OUTPUT_OPEN, OUTPUT_NONE } swapp_output_state;

static CRITICAL_SECTION g_lock; /* the status fields below */
static int g_locks_ready = 0;
static HANDLE g_thread = NULL;
static HANDLE g_render_thread = NULL;
static HANDLE g_stop_event = NULL;
static HANDLE g_change_event = NULL;
static volatile LONG g_stop = 0;
static int g_ok = 0;
static char g_text[SWAPP_AUDIO_STATUS_MAX] = "Audio: waiting for the client";
static void (*g_state_callback)(void) = NULL;

/* What each thread reports, combined into the status text. */
static int g_udp_failed = 0;
static int g_connected = 0;
static uint32_t g_peer_be = 0;
static ULONGLONG g_last_accept = 0; /* 0: nothing accepted from the current client yet */
static swapp_output_state g_output = OUTPUT_PENDING;
static char g_output_name[96] = "";
static int g_buffered_ms = -1; /* the drift controller's smoothed delay, -1 until it has one */
static int g_no_drift = 0;     /* the device refused rate adjustment */

/* About 80 KB: far too big for a thread stack, so it lives here. */
static CRITICAL_SECTION g_jitter_lock;
static swapp_jitter g_jitter;

/* Appends " · N ms buffered". The tray shows the status with SetWindowTextA, so
 * the dot goes through the ANSI code page like the device name does; a code
 * page without it gets a plain hyphen. */
static void swapp_audio_append_buffered(char *text, size_t size, size_t at, int ms) {
    char sep[16];
    BOOL used = FALSE;
    /* used reports a character the code page replaced with a default; it must
     * be NULL for UTF-8, where the parameter is not allowed. */
    int r = WideCharToMultiByte(CP_ACP, 0, L" \u00b7 ", -1, sep, (int)sizeof sep, NULL,
                                GetACP() == CP_UTF8 ? NULL : &used);
    if (!r || used) {
        strcpy(sep, " - ");
    }
    _snprintf_s(text + at, size - at, _TRUNCATE, "%s%d ms buffered", sep, ms);
}

/* Rebuilds the status from both threads' state, and runs the callback only
 * when the text changed. Built and stored under one lock, so two threads
 * refreshing at once cannot leave an older text behind. Like the net module,
 * the callback runs on the calling thread and the tray marshals it to the UI
 * thread. */
static void swapp_audio_refresh_status(void) {
    EnterCriticalSection(&g_lock);
    char text[sizeof g_text];
    int ok = 0;
    if (g_udp_failed) {
        snprintf(text, sizeof text, "Audio: UDP port 21423 unavailable");
    } else if (g_output == OUTPUT_NONE) {
        snprintf(text, sizeof text, "Audio: no output device");
    } else if (!g_connected) {
        snprintf(text, sizeof text, "Audio: waiting for the client");
    } else {
        char ip[INET_ADDRSTRLEN];
        struct in_addr addr = {.s_addr = g_peer_be};
        inet_ntop(AF_INET, &addr, ip, sizeof ip);
        if (g_last_accept != 0 && GetTickCount64() - g_last_accept < SWAPP_AUDIO_SILENT_MS) {
            ok = 1;
            if (g_output == OUTPUT_OPEN) {
                size_t n = (size_t)snprintf(text, sizeof text, "Audio: playing on %s", g_output_name);
                if (g_buffered_ms >= 0 && n < sizeof text) {
                    swapp_audio_append_buffered(text, sizeof text, n, g_buffered_ms);
                }
                if (g_no_drift && strlen(text) + 20 < sizeof text) {
                    strcat(text, " (no drift control)");
                }
            } else {
                snprintf(text, sizeof text, "Audio: receiving from %s", ip);
            }
        } else {
            snprintf(text, sizeof text, "Audio: nothing from %s over UDP", ip);
        }
    }
    int changed = strcmp(text, g_text) != 0;
    if (changed) {
        _snprintf_s(g_text, sizeof g_text, _TRUNCATE, "%s", text);
    }
    g_ok = ok;
    LeaveCriticalSection(&g_lock);
    if (changed && g_state_callback) {
        g_state_callback();
    }
}

static void swapp_audio_set_drift_status(int buffered_ms, int no_drift) {
    EnterCriticalSection(&g_lock);
    g_buffered_ms = buffered_ms;
    g_no_drift = no_drift;
    LeaveCriticalSection(&g_lock);
    swapp_audio_refresh_status();
}

static void swapp_audio_set_output(swapp_output_state state, const char *name) {
    EnterCriticalSection(&g_lock);
    g_output = state;
    _snprintf_s(g_output_name, sizeof g_output_name, _TRUNCATE, "%s", name ? name : "");
    LeaveCriticalSection(&g_lock);
    swapp_audio_refresh_status();
}

/* ---- Receiving ---- */

static SOCKET swapp_audio_bind(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SWAPP_NET_PORT);
    DWORD timeout_ms = 200;
    /* The timeout is what lets the loop notice a stop and re-evaluate the
     * status as time passes. */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof timeout_ms);
    /* Room for about a second of datagrams, so a moment without CPU for this
     * thread delays audio instead of dropping it. */
    int rcvbuf = 256 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof rcvbuf);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static void swapp_audio_udp_failed(void) {
    EnterCriticalSection(&g_lock);
    g_udp_failed = 1;
    LeaveCriticalSection(&g_lock);
    swapp_audio_refresh_status();
}

static DWORD WINAPI swapp_audio_thread(LPVOID param) {
    (void)param;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        swapp_audio_udp_failed();
        return 0;
    }
    SOCKET sock = swapp_audio_bind();
    if (sock == INVALID_SOCKET) {
        /* Not fatal for the app, but permanent for audio: nothing retries. */
        swapp_audio_udp_failed();
        WSACleanup();
        return 0;
    }

    /* This thread shares the jitter lock with the render thread. At normal
     * priority, a busy CPU (a video encode) preempts it while it holds the
     * lock or before it drains the socket, and the render thread runs dry.
     * Same MMCSS class as the render thread; may fail like it. */
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    ULONGLONG last_accept = 0; /* 0: nothing accepted from the current client yet */
    uint32_t last_peer = 0;

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        uint8_t buf[SWAPP_AUDIO_HEADER_SIZE + SWAPP_AUDIO_MAX_PAYLOAD + 1];
        struct sockaddr_in from;
        int from_len = sizeof from;
        int n = recvfrom(sock, (char *)buf, sizeof buf, 0, (struct sockaddr *)&from, &from_len);

        uint32_t peer_be = 0, self_be = 0;
        int connected = swapp_net_peer(&peer_be, &self_be);
        if (!connected || peer_be != last_peer) {
            last_accept = 0;
            last_peer = connected ? peer_be : 0;
        }

        if (n > 0 && connected && from.sin_addr.s_addr == peer_be) {
            swapp_audio_packet pkt;
            if (swapp_audio_packet_parse(buf, (size_t)n, &pkt)) {
                last_accept = GetTickCount64();
                EnterCriticalSection(&g_jitter_lock);
                swapp_jitter_push(&g_jitter, pkt.stream_id, pkt.seq, pkt.payload, pkt.payload_len);
                LeaveCriticalSection(&g_jitter_lock);
            }
        }

        EnterCriticalSection(&g_lock);
        g_connected = connected;
        g_peer_be = last_peer;
        g_last_accept = last_accept;
        LeaveCriticalSection(&g_lock);
        swapp_audio_refresh_status();
    }

    if (task) {
        AvRevertMmThreadCharacteristics(task);
    }
    closesocket(sock);
    WSACleanup();
    return 0;
}

/* ---- Following the default output ---- */

/* A C implementation of IMMNotificationClient. It is a single static object,
 * so the reference count is pinned at 1. */
static HRESULT STDMETHODCALLTYPE swapp_notify_query(IMMNotificationClient *self, REFIID riid,
                                                    void **out) {
    if (IsEqualGUID(riid, &swapp_iid_unknown) || IsEqualGUID(riid, &swapp_iid_notification)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE swapp_notify_add_ref(IMMNotificationClient *self) {
    (void)self;
    return 1;
}

static ULONG STDMETHODCALLTYPE swapp_notify_release(IMMNotificationClient *self) {
    (void)self;
    return 1;
}

static HRESULT STDMETHODCALLTYPE swapp_notify_state(IMMNotificationClient *self, LPCWSTR id,
                                                    DWORD state) {
    (void)self;
    (void)id;
    (void)state;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE swapp_notify_added(IMMNotificationClient *self, LPCWSTR id) {
    (void)self;
    (void)id;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE swapp_notify_removed(IMMNotificationClient *self, LPCWSTR id) {
    (void)self;
    (void)id;
    return S_OK;
}

/* Runs on a system thread, so it only signals the render thread. */
static HRESULT STDMETHODCALLTYPE swapp_notify_default(IMMNotificationClient *self, EDataFlow flow,
                                                      ERole role, LPCWSTR id) {
    (void)self;
    (void)id;
    if (flow == eRender && role == eConsole) {
        SetEvent(g_change_event);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE swapp_notify_property(IMMNotificationClient *self, LPCWSTR id,
                                                       const PROPERTYKEY key) {
    (void)self;
    (void)id;
    (void)key;
    return S_OK;
}

static IMMNotificationClientVtbl g_notify_vtbl = {
    .QueryInterface = swapp_notify_query,
    .AddRef = swapp_notify_add_ref,
    .Release = swapp_notify_release,
    .OnDeviceStateChanged = swapp_notify_state,
    .OnDeviceAdded = swapp_notify_added,
    .OnDeviceRemoved = swapp_notify_removed,
    .OnDefaultDeviceChanged = swapp_notify_default,
    .OnPropertyValueChanged = swapp_notify_property,
};
static IMMNotificationClient g_notify = {&g_notify_vtbl};

/* ---- Playing ---- */

typedef struct {
    IMMDeviceEnumerator *enumerator;
    int registered;
    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render;
    IAudioClockAdjustment *adjust; /* NULL when the stream has no rate adjustment */
    HANDLE event;
    UINT32 buffer_frames;
    OpusDecoder *decoder;
    uint32_t stream; /* the jitter buffer's stream the decoder last saw */
    swapp_drift drift;
    int frames;       /* popped since the drift last moved the rate */
    int rate_nudged;  /* the device is not at the nominal rate */
} swapp_output;

/* Back to the nominal rate and an empty history, for when the buffer is
 * refilling or a new sender starts: the old delay says nothing about it. */
static void swapp_output_drift_reset(swapp_output *o) {
    int was_shown = o->drift.primed;
    swapp_drift_reset(&o->drift);
    o->frames = 0;
    if (o->adjust && o->rate_nudged) {
        IAudioClockAdjustment_SetSampleRate(o->adjust, (float)SWAPP_AUDIO_RATE);
    }
    o->rate_nudged = 0;
    if (was_shown) {
        swapp_audio_set_drift_status(-1, o->adjust == NULL);
    }
}

/* Decodes the next 10 ms into pcm. Returns 0 when there is nothing to play,
 * and the caller writes silence instead. */
static int swapp_output_decode(swapp_output *o, float *pcm, UINT32 padding) {
    uint8_t payload[SWAPP_AUDIO_MAX_PAYLOAD];
    size_t len = 0;
    EnterCriticalSection(&g_jitter_lock);
    int depth = swapp_jitter_depth(&g_jitter);
    swapp_jitter_result result = swapp_jitter_pop(&g_jitter, payload, &len);
    uint32_t stream = swapp_jitter_stream(&g_jitter);
    LeaveCriticalSection(&g_jitter_lock);

    if (result == SWAPP_JITTER_SILENCE || stream != o->stream) {
        swapp_output_drift_reset(o);
    } else {
        /* Frames still in the jitter buffer plus what the device holds. */
        double fill_ms = depth * 10.0 + padding * 1000.0 / SWAPP_AUDIO_RATE;
        /* The jitter target plus the device buffer that is actually held. */
        double target_ms = SWAPP_AUDIO_TARGET_MS + o->buffer_frames * 1000.0 / SWAPP_AUDIO_RATE;
        double ratio = swapp_drift_update(&o->drift, fill_ms, target_ms);
        if (++o->frames >= DRIFT_STEP_FRAMES) {
            o->frames = 0;
            if (o->adjust) {
                if (SUCCEEDED(IAudioClockAdjustment_SetSampleRate(
                        o->adjust, (float)(SWAPP_AUDIO_RATE * ratio)))) {
                    o->rate_nudged = 1;
                } else {
                    /* Without a working rate there is no drift control, and
                     * the status says so until the next open. */
                    IAudioClockAdjustment_Release(o->adjust);
                    o->adjust = NULL;
                    o->rate_nudged = 0;
                }
            }
            swapp_audio_set_drift_status((int)(o->drift.smoothed_ms + 0.5), o->adjust == NULL);
        }
    }

    if (!o->decoder) {
        return 0;
    }
    if (stream != o->stream) {
        /* A new sender: the previous one's decoder history would only
         * smear into its first frames. */
        opus_decoder_ctl(o->decoder, OPUS_RESET_STATE);
        o->stream = stream;
    }
    int samples;
    if (result == SWAPP_JITTER_PACKET) {
        samples = opus_decode_float(o->decoder, payload, (opus_int32)len, pcm, SWAPP_AUDIO_FRAME, 0);
    } else if (result == SWAPP_JITTER_LOST) {
        samples = opus_decode_float(o->decoder, NULL, 0, pcm, SWAPP_AUDIO_FRAME, 0);
    } else {
        return 0;
    }
    return samples == SWAPP_AUDIO_FRAME;
}

/* Fills whatever room the device buffer has, in whole 10 ms frames. */
static HRESULT swapp_output_feed(swapp_output *o) {
    UINT32 padding = 0;
    HRESULT hr = IAudioClient_GetCurrentPadding(o->client, &padding);
    if (FAILED(hr)) {
        return hr;
    }
    while (o->buffer_frames - padding >= SWAPP_AUDIO_FRAME) {
        BYTE *data = NULL;
        hr = IAudioRenderClient_GetBuffer(o->render, SWAPP_AUDIO_FRAME, &data);
        if (FAILED(hr)) {
            return hr;
        }
        int ok = swapp_output_decode(o, (float *)data, padding);
        hr = IAudioRenderClient_ReleaseBuffer(o->render, SWAPP_AUDIO_FRAME,
                                              ok ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(hr)) {
            return hr;
        }
        padding += SWAPP_AUDIO_FRAME;
    }
    return S_OK;
}

/* Releases the stream and the device, keeping the enumerator, its
 * registration and the decoder. */
static void swapp_output_close(swapp_output *o) {
    if (o->client) {
        IAudioClient_Stop(o->client);
    }
    if (o->render) {
        IAudioRenderClient_Release(o->render);
        o->render = NULL;
    }
    if (o->adjust) {
        IAudioClockAdjustment_Release(o->adjust);
        o->adjust = NULL;
    }
    if (o->client) {
        IAudioClient_Release(o->client);
        o->client = NULL;
    }
    if (o->device) {
        IMMDevice_Release(o->device);
        o->device = NULL;
    }
}

static void swapp_output_name(IMMDevice *device, char *name, size_t size) {
    snprintf(name, size, "the default output");
    IPropertyStore *store = NULL;
    if (FAILED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store))) {
        return;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_Device_FriendlyName, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        /* The ANSI code page rather than UTF-8: the tray shows this text with
         * SetWindowTextA, which reads it in that code page. */
        if (!WideCharToMultiByte(CP_ACP, 0, value.pwszVal, -1, name, (int)size, NULL, NULL)) {
            snprintf(name, size, "the default output");
        }
    }
    PropVariantClear(&value);
    IPropertyStore_Release(store);
}

/* Opens the current default output and starts it. Returns 0, with nothing
 * left open, when there is no usable device. */
static int swapp_output_open(swapp_output *o, char *name, size_t name_size) {
    HRESULT hr;
    if (!o->enumerator) {
        hr = CoCreateInstance(&swapp_clsid_enumerator, NULL, CLSCTX_ALL, &swapp_iid_enumerator,
                              (void **)&o->enumerator);
        if (FAILED(hr)) {
            o->enumerator = NULL;
            return 0;
        }
        o->registered = SUCCEEDED(
            IMMDeviceEnumerator_RegisterEndpointNotificationCallback(o->enumerator, &g_notify));
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(o->enumerator, eRender, eConsole, &o->device);
    if (FAILED(hr)) {
        o->device = NULL;
        return 0;
    }
    swapp_output_name(o->device, name, name_size);

    /* Exactly what the decoder produces; the shared-mode engine converts it
     * to whatever the device mixes in, so the device's format never
     * matters here. */
    WAVEFORMATEXTENSIBLE format = {0};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = SWAPP_AUDIO_CHANNELS;
    format.Format.nSamplesPerSec = SWAPP_AUDIO_RATE;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = (WORD)(SWAPP_AUDIO_CHANNELS * sizeof(float));
    format.Format.nAvgBytesPerSec = (DWORD)SWAPP_AUDIO_RATE * format.Format.nBlockAlign;
    format.Format.cbSize = (WORD)(sizeof format - sizeof format.Format);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = swapp_subtype_float;

    /* Rate adjustment is what holds the delay steady; a device that refuses it
     * still plays, just without drift control. A client that failed to
     * initialize is not reusable, so the retry activates a fresh one. */
    int no_drift = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (attempt == 0) {
            flags |= AUDCLNT_STREAMFLAGS_RATEADJUST;
        } else {
            no_drift = 1;
        }
        hr = IMMDevice_Activate(o->device, &swapp_iid_audio_client, CLSCTX_ALL, NULL,
                                (void **)&o->client);
        if (FAILED(hr)) {
            o->client = NULL;
            swapp_output_close(o);
            return 0;
        }
        hr = IAudioClient_Initialize(o->client, AUDCLNT_SHAREMODE_SHARED, flags, BUFFER_HNS, 0,
                                     &format.Format, NULL);
        if (SUCCEEDED(hr) || attempt == 1) {
            break;
        }
        IAudioClient_Release(o->client);
        o->client = NULL;
    }
    if (SUCCEEDED(hr) && !no_drift) {
        if (FAILED(IAudioClient_GetService(o->client, &swapp_iid_clock_adjustment,
                                           (void **)&o->adjust))) {
            o->adjust = NULL;
            no_drift = 1;
        }
    }
    if (SUCCEEDED(hr)) {
        swapp_output_drift_reset(o);
        swapp_audio_set_drift_status(-1, no_drift);
    }
    if (SUCCEEDED(hr)) {
        hr = IAudioClient_SetEventHandle(o->client, o->event);
    }
    if (SUCCEEDED(hr)) {
        hr = IAudioClient_GetBufferSize(o->client, &o->buffer_frames);
    }
    if (SUCCEEDED(hr)) {
        hr = IAudioClient_GetService(o->client, &swapp_iid_render_client, (void **)&o->render);
        if (FAILED(hr)) {
            o->render = NULL;
        }
    }
    if (SUCCEEDED(hr)) {
        /* Whatever piled up while there was no stream would otherwise stay
         * as added latency for good: start again from the target. */
        EnterCriticalSection(&g_jitter_lock);
        swapp_jitter_init(&g_jitter, TARGET_FRAMES);
        LeaveCriticalSection(&g_jitter_lock);
        /* Filled before Start, so the device does not begin on an empty
         * buffer. */
        hr = swapp_output_feed(o);
    }
    if (SUCCEEDED(hr)) {
        hr = IAudioClient_Start(o->client);
    }
    if (FAILED(hr)) {
        swapp_output_close(o);
        return 0;
    }
    return 1;
}

static DWORD WINAPI swapp_audio_render_thread(LPVOID param) {
    (void)param;
    HRESULT com = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DWORD task_index = 0;
    /* May fail (the MMCSS service can be off); playback works without it,
     * just with less protection from other load. */
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    swapp_output out = {0};
    int err = 0;
    out.decoder = opus_decoder_create(SWAPP_AUDIO_RATE, SWAPP_AUDIO_CHANNELS, &err);
    if (err != OPUS_OK) {
        out.decoder = NULL; /* plays silence rather than nothing */
    }
    out.event = CreateEventW(NULL, FALSE, FALSE, NULL);

    HANDLE waits[3] = {out.event, g_stop_event, g_change_event};
    int open = 0;
    DWORD pause = 0; /* before the next open attempt */

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        if (!open) {
            if (pause) {
                /* Woken early by stop, or by a new default to try at once. */
                WaitForMultipleObjects(2, waits + 1, FALSE, pause);
                if (InterlockedCompareExchange(&g_stop, 0, 0)) {
                    break;
                }
            }
            char name[96];
            if (SUCCEEDED(com) && out.event && swapp_output_open(&out, name, sizeof name)) {
                open = 1;
                swapp_audio_set_output(OUTPUT_OPEN, name);
            } else {
                swapp_audio_set_output(OUTPUT_NONE, NULL);
                pause = RETRY_MS;
                continue;
            }
        }

        DWORD w = WaitForMultipleObjects(3, waits, FALSE, RENDER_WAIT_MS);
        if (w == WAIT_OBJECT_0 + 1) {
            break;
        }
        HRESULT hr;
        if (w == WAIT_OBJECT_0 + 2) {
            /* A new default: the open stream is now the wrong device, which
             * is handled exactly like losing it. */
            hr = AUDCLNT_E_DEVICE_INVALIDATED;
            pause = 0;
        } else {
            /* The render event, or the timeout: either way the device is
             * asked, and a vanished one answers with an error. Any failure,
             * AUDCLNT_E_DEVICE_INVALIDATED or otherwise, leaves the stream
             * unusable, so all of them reopen. */
            hr = swapp_output_feed(&out);
            pause = REOPEN_PAUSE_MS;
        }
        if (FAILED(hr)) {
            swapp_output_close(&out);
            open = 0;
        }
    }

    swapp_output_close(&out);
    if (out.enumerator) {
        if (out.registered) {
            IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(out.enumerator, &g_notify);
        }
        IMMDeviceEnumerator_Release(out.enumerator);
    }
    if (out.event) {
        CloseHandle(out.event);
    }
    if (out.decoder) {
        opus_decoder_destroy(out.decoder);
    }
    if (task) {
        AvRevertMmThreadCharacteristics(task);
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return 0;
}

/* ---- Public ---- */

void swapp_audio_start(void) {
    if (g_thread || g_render_thread) {
        return;
    }
    if (!g_locks_ready) {
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_jitter_lock);
        g_locks_ready = 1;
    }
    swapp_jitter_init(&g_jitter, TARGET_FRAMES);
    InterlockedExchange(&g_stop, 0);
    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_change_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, swapp_audio_thread, NULL, 0, NULL);
    if (g_stop_event && g_change_event) {
        /* With no events to wait on, the server still receives; it just
         * cannot play. */
        g_render_thread = CreateThread(NULL, 0, swapp_audio_render_thread, NULL, 0, NULL);
    }
}

void swapp_audio_stop(int remove_device) {
    /* Nothing to remove on the server. */
    (void)remove_device;
    if (!g_thread && !g_render_thread) {
        return;
    }
    InterlockedExchange(&g_stop, 1);
    if (g_stop_event) {
        SetEvent(g_stop_event);
    }
    HANDLE threads[2];
    DWORD count = 0;
    if (g_thread) {
        threads[count++] = g_thread;
    }
    if (g_render_thread) {
        threads[count++] = g_render_thread;
    }
    /* One wait for both, so the 500 ms covers them together. The receiver
     * wakes within its 200 ms receive timeout and the render thread at once,
     * and if either is stuck anyway shutdown must not hang on it. */
    DWORD w = WaitForMultipleObjects(count, threads, TRUE, STOP_WAIT_MS);
    int finished = w < WAIT_OBJECT_0 + count;
    for (DWORD i = 0; i < count; i++) {
        CloseHandle(threads[i]);
    }
    g_thread = NULL;
    g_render_thread = NULL;
    if (finished) {
        /* Only now: a thread left behind may still wait on them. */
        if (g_stop_event) {
            CloseHandle(g_stop_event);
        }
        if (g_change_event) {
            CloseHandle(g_change_event);
        }
        g_stop_event = NULL;
        g_change_event = NULL;
    }
}

int swapp_audio_ok(void) {
    EnterCriticalSection(&g_lock);
    int ok = g_ok;
    LeaveCriticalSection(&g_lock);
    return ok;
}

int swapp_audio_failed(void) {
    EnterCriticalSection(&g_lock);
    int failed = g_udp_failed;
    LeaveCriticalSection(&g_lock);
    return failed;
}

void swapp_audio_status_text(char *buf, size_t buf_size) {
    EnterCriticalSection(&g_lock);
    _snprintf_s(buf, buf_size, _TRUNCATE, "%s", g_text);
    LeaveCriticalSection(&g_lock);
}

void swapp_audio_set_state_callback(void (*callback)(void)) {
    g_state_callback = callback;
}

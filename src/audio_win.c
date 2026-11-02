/* Audio relay, server side. For now: receive the client's UDP datagrams and
 * report whether any arrive, which proves the path through this PC's
 * firewall. Real audio replaces the datagram handling later. */

#include "audio.h"
#include "audio_packet.h"
#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <string.h>
#include <windows.h>

#define STOP_WAIT_MS 500

static CRITICAL_SECTION g_lock;
static HANDLE g_thread = NULL;
static volatile LONG g_stop = 0;
static int g_ok = 0;
static char g_text[96] = "Audio: waiting for the client";
static void (*g_state_callback)(void) = NULL;

/* Stores the text and ok flag, and runs the callback only when the text
 * changed. Like the net module, the callback runs on this thread and the tray
 * marshals it to the UI thread. */
static void swapp_audio_set_status(const char *text, int ok) {
    EnterCriticalSection(&g_lock);
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
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static DWORD WINAPI swapp_audio_thread(LPVOID param) {
    (void)param;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        swapp_audio_set_status("Audio: UDP port 21423 unavailable", 0);
        return 0;
    }
    SOCKET sock = swapp_audio_bind();
    if (sock == INVALID_SOCKET) {
        /* Not fatal for the app, but permanent for audio: nothing retries. */
        swapp_audio_set_status("Audio: UDP port 21423 unavailable", 0);
        WSACleanup();
        return 0;
    }

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
            }
        }

        char text[96];
        int ok = 0;
        if (!connected) {
            snprintf(text, sizeof text, "Audio: waiting for the client");
        } else {
            char ip[INET_ADDRSTRLEN];
            struct in_addr addr = {.s_addr = peer_be};
            inet_ntop(AF_INET, &addr, ip, sizeof ip);
            if (last_accept != 0 && GetTickCount64() - last_accept < SWAPP_AUDIO_SILENT_MS) {
                snprintf(text, sizeof text, "Audio: receiving from %s", ip);
                ok = 1;
            } else {
                snprintf(text, sizeof text, "Audio: nothing from %s over UDP", ip);
            }
        }
        swapp_audio_set_status(text, ok);
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}

void swapp_audio_start(void) {
    if (g_thread) {
        return;
    }
    InitializeCriticalSection(&g_lock);
    InterlockedExchange(&g_stop, 0);
    g_thread = CreateThread(NULL, 0, swapp_audio_thread, NULL, 0, NULL);
}

void swapp_audio_stop(int remove_device) {
    /* Nothing to remove on the server. */
    (void)remove_device;
    if (!g_thread) {
        return;
    }
    InterlockedExchange(&g_stop, 1);
    /* Bounded: the thread wakes within its 200 ms receive timeout, and if it
     * is stuck anyway shutdown must not hang on it. */
    WaitForSingleObject(g_thread, STOP_WAIT_MS);
    CloseHandle(g_thread);
    g_thread = NULL;
}

int swapp_audio_ok(void) {
    EnterCriticalSection(&g_lock);
    int ok = g_ok;
    LeaveCriticalSection(&g_lock);
    return ok;
}

void swapp_audio_status_text(char *buf, size_t buf_size) {
    EnterCriticalSection(&g_lock);
    _snprintf_s(buf, buf_size, _TRUNCATE, "%s", g_text);
    LeaveCriticalSection(&g_lock);
}

void swapp_audio_set_state_callback(void (*callback)(void)) {
    g_state_callback = callback;
}

/* Windows side of the link: the server. Owns the monitors, so it also owns
 * the listening socket -- it is the half that stays put while the client
 * comes and goes. One client at a time; a second connection is refused
 * outright rather than queued, because two clients issuing switches would
 * be the split-brain this design exists to prevent.
 *
 * Everything here runs on one dedicated thread; the UI only reads the
 * state through swapp_net_get_state(). */

#include "net.h"
#include "monitors.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <string.h>
#include <windows.h>

static CRITICAL_SECTION g_state_lock;
static swapp_net_state g_state = SWAPP_NET_WAITING;
static char g_peer[64] = "";
static volatile LONG g_stop = 0;
static void (*g_state_callback)(void) = NULL;
static void (*g_rescan_callback)(void) = NULL;
static void (*g_switch_callback)(int, int) = NULL;
static void (*g_assign_callback)(int, int, int) = NULL;
static void (*g_switch_all_callback)(int) = NULL;

/* The live client socket, so the UI thread can push the monitor list the
 * moment a scan finishes instead of waiting for the socket thread to come
 * back around. Guarded by the same lock as the state: both threads send on
 * it, and interleaved writes would splice two messages into nonsense. */
static SOCKET g_client = INVALID_SOCKET;
static CRITICAL_SECTION g_send_lock;

/* Sends one whole line. Callers hold g_send_lock. */
static int swapp_net_send_line(SOCKET sock, const char *line) {
    int length = (int)strlen(line);
    return send(sock, line, length, 0) == length;
}

static void swapp_net_set_state(swapp_net_state state, const char *peer) {
    EnterCriticalSection(&g_state_lock);
    int changed = (g_state != state) || (peer && strcmp(g_peer, peer) != 0);
    g_state = state;
    if (peer) {
        strncpy(g_peer, peer, sizeof(g_peer) - 1);
        g_peer[sizeof(g_peer) - 1] = '\0';
    } else {
        g_peer[0] = '\0';
    }
    LeaveCriticalSection(&g_state_lock);

    if (changed && g_state_callback) {
        /* The tray's message loop is the UI thread; the callback is
         * expected to marshal there itself (PostMessage), so calling it
         * from the socket thread is safe. */
        g_state_callback();
    }
}

/* Binds the listener. Returns INVALID_SOCKET if the port is taken, which
 * is terminal: another copy of Swapp is already serving, and starting a
 * second one would give the client two authorities to talk to. */
static SOCKET swapp_net_listen(void) {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    /* Deliberately NOT SO_REUSEADDR: on Windows that lets a second process
     * steal a live socket, turning the "port already in use" error this
     * relies on into a silent double bind. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SWAPP_NET_PORT);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR
        || listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        return INVALID_SOCKET;
    }
    return listener;
}

/* Pumps one client connection until it dies. Nothing but keepalive travels
 * over it yet; commands land here once the client can drive the monitor
 * state. */
static void swapp_net_serve(SOCKET listener, SOCKET client, const char *peer) {
    EnterCriticalSection(&g_send_lock);
    g_client = client;
    LeaveCriticalSection(&g_send_lock);

    swapp_net_set_state(SWAPP_NET_CONNECTED, peer);

    /* A client that connects after the scan already ran gets the list at
     * once; one that connects before it gets nothing here and the list
     * arrives from the scan's own completion. */
    swapp_net_send_monitors();

    /* Bounds how long a recv blocks, so the timeout below is checked even
     * on a silent socket. */
    DWORD timeout = 1000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    BOOL nodelay = TRUE;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

    ULONGLONG last_received = GetTickCount64();
    ULONGLONG last_ping = 0;
    char buffer[512];
    /* Partial line carried between reads: TCP gives no message boundaries. */
    char pending[512];
    size_t pending_len = 0;

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        ULONGLONG now = GetTickCount64();

        if (now - last_ping >= SWAPP_NET_PING_SECONDS * 1000ULL) {
            EnterCriticalSection(&g_send_lock);
            int sent = swapp_net_send_line(client, "ping\n");
            LeaveCriticalSection(&g_send_lock);
            if (!sent) {
                break;
            }
            last_ping = now;
        }

        int received = recv(client, buffer, sizeof(buffer), 0);
        if (received > 0) {
            /* Any traffic proves the peer is alive. */
            last_received = GetTickCount64();

            int alive = 1;
            for (int i = 0; i < received && alive; i++) {
                char ch = buffer[i];
                if (ch != '\n') {
                    if (ch != '\r' && pending_len < sizeof(pending) - 1) {
                        pending[pending_len++] = ch;
                    }
                    continue;
                }
                pending[pending_len] = '\0';
                pending_len = 0;

                if (strcmp(pending, "ping") == 0) {
                    EnterCriticalSection(&g_send_lock);
                    alive = swapp_net_send_line(client, "pong\n");
                    LeaveCriticalSection(&g_send_lock);
                } else if (strcmp(pending, "rescan") == 0) {
                    if (g_rescan_callback) {
                        g_rescan_callback();
                    }
                } else if (strncmp(pending, "switchall ", 10) == 0) {
                    const char *which = pending + 10;
                    int role = strcmp(which, "windows") == 0 ? SWAPP_ROLE_WINDOWS
                               : strcmp(which, "linux") == 0 ? SWAPP_ROLE_LINUX
                                                             : -1;
                    if (role >= 0 && g_switch_all_callback) {
                        g_switch_all_callback(role);
                    }
                } else if (strncmp(pending, "assign ", 7) == 0) {
                    int monitor_index = -1;
                    int input_code = -1;
                    char role_name[16];
                    if (sscanf_s(pending, "assign %d %d %15s", &monitor_index, &input_code,
                                 role_name, (unsigned)sizeof(role_name)) == 3
                        && g_assign_callback) {
                        int role = strcmp(role_name, "windows") == 0 ? SWAPP_ROLE_WINDOWS
                                   : strcmp(role_name, "linux") == 0 ? SWAPP_ROLE_LINUX
                                   : strcmp(role_name, "none") == 0  ? SWAPP_ROLE_NONE
                                                                      : -1;
                        if (role >= 0) {
                            g_assign_callback(monitor_index, input_code, role);
                        }
                    }
                } else if (strncmp(pending, "switch ", 7) == 0) {
                    int monitor_index = -1;
                    int input_code = -1;
                    if (sscanf_s(pending, "switch %d %d", &monitor_index, &input_code) == 2
                        && g_switch_callback) {
                        g_switch_callback(monitor_index, input_code);
                    }
                }
                /* Anything else is ignored, so an older server survives a
                 * newer client. */
            }
            if (!alive) {
                break;
            }
        } else if (received == 0) {
            break; /* clean close by the client */
        } else if (WSAGetLastError() != WSAETIMEDOUT) {
            break;
        }

        if (GetTickCount64() - last_received >= SWAPP_NET_TIMEOUT_SECONDS * 1000ULL) {
            break; /* client went away without saying so */
        }

        /* One client only. Anyone else knocking is told so and dropped
         * immediately, rather than being left to sit in the backlog where
         * it would silently inherit the link the moment this one ends. */
        SOCKET extra = accept(listener, NULL, NULL);
        if (extra != INVALID_SOCKET) {
            send(extra, "err busy\n", 9, 0);
            closesocket(extra);
        }
    }

    EnterCriticalSection(&g_send_lock);
    g_client = INVALID_SOCKET;
    LeaveCriticalSection(&g_send_lock);

    closesocket(client);
}

/* Pushes the monitor cache to the connected client, if any. Called from
 * the UI thread when a scan finishes, and from the socket thread when a
 * client arrives. */
void swapp_net_send_monitors(void) {
    EnterCriticalSection(&g_send_lock);
    SOCKET client = g_client;
    if (client == INVALID_SOCKET) {
        LeaveCriticalSection(&g_send_lock);
        return;
    }

    char line[512];
    size_t count = swapp_monitors_count();

    /* An empty cache means the scan hasn't finished, not that the machine
     * has no monitors -- a client that connects while the server is still
     * starting up must keep waiting rather than be told there are none. */
    if (count == 0 || swapp_monitors_busy()) {
        swapp_net_send_line(client, "scanning\n");
        LeaveCriticalSection(&g_send_lock);
        return;
    }

    _snprintf_s(line, sizeof(line), _TRUNCATE, "monitors %zu\n", count);

    int ok = swapp_net_send_line(client, line);
    for (size_t i = 0; i < count && ok; i++) {
        char label[128];
        swapp_monitors_label(i, label, sizeof(label));

        _snprintf_s(line, sizeof(line), _TRUNCATE, "monitor %zu %d %s\n", i,
                    swapp_monitors_active_input(i), label);
        ok = swapp_net_send_line(client, line);

        /* The server's own input on this monitor, when known. */
        int windows_code = swapp_roles_code_for(label, SWAPP_ROLE_WINDOWS);
        if (ok && windows_code >= 0) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "windows %zu %d\n", i, windows_code);
            ok = swapp_net_send_line(client, line);
        }
        int linux_code = swapp_roles_code_for(label, SWAPP_ROLE_LINUX);
        if (ok && linux_code >= 0) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "linux %zu %d\n", i, linux_code);
            ok = swapp_net_send_line(client, line);
        }

        int codes[64];
        int n_codes = swapp_monitors_inputs(i, codes, (int)(sizeof(codes) / sizeof(codes[0])));
        for (int c = 0; c < n_codes && ok; c++) {
            _snprintf_s(line, sizeof(line), _TRUNCATE, "input %zu %d %s\n", i, codes[c],
                        swapp_monitors_input_name(codes[c]));
            ok = swapp_net_send_line(client, line);
        }
    }

    if (ok) {
        /* The client shows nothing until this lands, so a list cut off
         * halfway is simply never displayed. */
        swapp_net_send_line(client, "end\n");
    }
    LeaveCriticalSection(&g_send_lock);
}

static DWORD WINAPI swapp_net_thread(LPVOID param) {
    (void)param;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        swapp_net_set_state(SWAPP_NET_FAILED, NULL);
        return 0;
    }

    SOCKET listener = swapp_net_listen();
    if (listener == INVALID_SOCKET) {
        swapp_net_set_state(SWAPP_NET_FAILED, NULL);
        WSACleanup();
        return 0;
    }

    /* Non-blocking accept so a quit doesn't have to wait for a client. */
    u_long nonblocking = 1;
    ioctlsocket(listener, FIONBIO, &nonblocking);

    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        swapp_net_set_state(SWAPP_NET_WAITING, NULL);

        struct sockaddr_in from;
        int from_len = sizeof(from);
        SOCKET client = accept(listener, (struct sockaddr *)&from, &from_len);
        if (client == INVALID_SOCKET) {
            Sleep(200);
            continue;
        }

        /* Back to blocking for the pump, which relies on SO_RCVTIMEO. */
        u_long blocking = 0;
        ioctlsocket(client, FIONBIO, &blocking);

        char peer[64] = "";
        inet_ntop(AF_INET, &from.sin_addr, peer, sizeof(peer));
        swapp_net_serve(listener, client, peer);
    }

    closesocket(listener);
    WSACleanup();
    swapp_net_set_state(SWAPP_NET_WAITING, NULL);
    return 0;
}

void swapp_net_start(void) {
    InitializeCriticalSection(&g_state_lock);
    InitializeCriticalSection(&g_send_lock);
    InterlockedExchange(&g_stop, 0);
    HANDLE thread = CreateThread(NULL, 0, swapp_net_thread, NULL, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    }
}

void swapp_net_stop(void) {
    InterlockedExchange(&g_stop, 1);
}

swapp_net_state swapp_net_get_state(void) {
    EnterCriticalSection(&g_state_lock);
    swapp_net_state state = g_state;
    LeaveCriticalSection(&g_state_lock);
    return state;
}

void swapp_net_status_text(char *buf, size_t buf_size) {
    EnterCriticalSection(&g_state_lock);
    if (g_state == SWAPP_NET_CONNECTED) {
        _snprintf_s(buf, buf_size, _TRUNCATE, "Client connected: %s", g_peer);
    } else if (g_state == SWAPP_NET_FAILED) {
        _snprintf_s(buf, buf_size, _TRUNCATE,
                    "Cannot listen on port %d -- is Swapp already running?", SWAPP_NET_PORT);
    } else {
        _snprintf_s(buf, buf_size, _TRUNCATE, "Waiting for client on port %d", SWAPP_NET_PORT);
    }
    LeaveCriticalSection(&g_state_lock);
}

void swapp_net_set_state_callback(void (*callback)(void)) {
    g_state_callback = callback;
}

void swapp_net_set_rescan_callback(void (*callback)(void)) {
    g_rescan_callback = callback;
}

void swapp_net_set_switch_callback(void (*callback)(int monitor_index, int input_code)) {
    g_switch_callback = callback;
}

void swapp_net_set_switch_all_callback(void (*callback)(int role)) {
    g_switch_all_callback = callback;
}

void swapp_net_request_switch_all(int role) {
    (void)role;
}

void swapp_net_send_acquire(void) {
    EnterCriticalSection(&g_send_lock);
    if (g_client != INVALID_SOCKET) {
        swapp_net_send_line(g_client, "acquire\n");
    }
    LeaveCriticalSection(&g_send_lock);
}

void swapp_net_set_assign_callback(void (*callback)(int monitor_index, int input_code, int role)) {
    g_assign_callback = callback;
}

void swapp_net_request_assign(int monitor_index, int input_code, int role) {
    (void)monitor_index;
    (void)input_code;
    (void)role;
}

/* The server runs its rescans and switches itself; there is nobody to ask. */
void swapp_net_request_rescan(void) {
}

void swapp_net_request_switch(int monitor_index, int input_code) {
    (void)monitor_index;
    (void)input_code;
}

/* Client-side entry points; the server holds the real cache, not a mirror
 * of someone else's. */
int swapp_net_monitors_ready(void) {
    return 0;
}

void swapp_net_monitors_text(char *buf, size_t buf_size) {
    if (buf_size > 0) {
        buf[0] = '\0';
    }
}

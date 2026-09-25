/* Linux side of the link: the client. Finds the Windows server by sweeping
 * the local subnets with outbound TCP connects, holds one connection to it,
 * and keeps that connection proven live with pings. Everything here runs on one dedicated thread;
 * the UI only ever reads the state through swapp_net_get_state(). */

#include "net.h"
#include "monitors.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <glib.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static GMutex g_state_lock;
static swapp_net_state g_state = SWAPP_NET_WAITING;
static char g_peer[64] = "";
static volatile int g_stop = 0;
static void (*g_state_callback)(void) = NULL;

/* The mirror of the server's monitor list. Guarded by the same lock as the
 * link state, and cleared with it: a list outlives its connection by
 * exactly nothing, since the server is the only thing that knows what the
 * monitors are doing. */
static char g_monitors[8192] = "";
static int g_monitors_ready = 0;

/* The live server socket, so the UI thread can send a rescan request. -1
 * with no connection. Writes from both threads go through g_send_lock so a
 * request can't splice into a pong. */
static int g_sock = -1;
static GMutex g_send_lock;

static int swapp_net_send_line(int sock, const char *line) {
    g_mutex_lock(&g_send_lock);
    size_t length = strlen(line);
    int ok = send(sock, line, length, MSG_NOSIGNAL) == (ssize_t)length;
    g_mutex_unlock(&g_send_lock);
    return ok;
}

static gboolean swapp_net_notify(gpointer user_data) {
    (void)user_data;
    if (g_state_callback) {
        g_state_callback();
    }
    return G_SOURCE_REMOVE;
}

static void swapp_net_set_state(swapp_net_state state, const char *peer) {
    g_mutex_lock(&g_state_lock);
    int changed = (g_state != state) || (peer && strcmp(g_peer, peer) != 0);
    g_state = state;
    if (state != SWAPP_NET_CONNECTED) {
        /* Dropping the link drops everything learned over it, so the UI
         * falls back to "waiting for a server" rather than showing a list
         * nothing stands behind any more. */
        g_monitors[0] = '\0';
        g_monitors_ready = 0;
    }
    if (peer) {
        g_strlcpy(g_peer, peer, sizeof(g_peer));
    } else {
        g_peer[0] = '\0';
    }
    g_mutex_unlock(&g_state_lock);

    /* The UI lives on the GTK main loop, so hop threads rather than
     * touching widgets from here. */
    if (changed) {
        g_idle_add(swapp_net_notify, NULL);
    }
}

/* Every /24 the machine has an address on, worth sweeping. Interfaces are
 * collected fresh each round so moving between networks is picked up
 * without a restart. Point-to-point interfaces (a VPN tun) are skipped:
 * their peers are not on our LAN, and sweeping one would mean hundreds of
 * connects into the corporate network. */
typedef struct {
    guint32 network_be;   /* the /24 base address, in network byte order */
    guint32 self_host;    /* our own host octet, skipped while sweeping */
    guint32 self_be;      /* our address on it, bound as the source */
} swapp_subnet;

/* Pins an outbound socket to a local address, so the packet leaves by the
 * interface that address belongs to. Without this the VPN's routes can
 * pull a connect onto tun0, sending probes for the home LAN into the
 * corporate network instead. */
static void swapp_net_bind_source(int sock, guint32 source_be) {
    struct sockaddr_in from = {0};
    from.sin_family = AF_INET;
    from.sin_addr.s_addr = source_be;
    bind(sock, (struct sockaddr *)&from, sizeof(from));
}

static int swapp_net_local_subnets(swapp_subnet *subnets, int max_subnets) {
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) {
        return 0;
    }

    int count = 0;
    for (struct ifaddrs *it = interfaces; it && count < max_subnets; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)
            || (it->ifa_flags & IFF_POINTOPOINT)) {
            continue;
        }

        guint32 addr = ntohl(((struct sockaddr_in *)it->ifa_addr)->sin_addr.s_addr);

        /* The server always lives on the home 192.168.x network. Anything
         * else the laptop is attached to -- a corporate 10.x, a hotspot's
         * 172.16.x -- is somebody else's network, and sweeping it would be
         * hundreds of unexplained connects into it. */
        if ((addr & 0xffff0000u) != 0xc0a80000u) {
            continue;
        }

        swapp_subnet subnet = {htonl(addr & 0xffffff00u), addr & 0xffu,
                               ((struct sockaddr_in *)it->ifa_addr)->sin_addr.s_addr};

        int duplicate = 0;
        for (int i = 0; i < count; i++) {
            duplicate |= (subnets[i].network_be == subnet.network_be);
        }
        if (!duplicate) {
            subnets[count++] = subnet;
        }
    }

    freeifaddrs(interfaces);
    return count;
}

/* One outbound connect attempt against a single host, waiting at most
 * timeout_ms. Returns a connected blocking socket, or -1. */
static int swapp_net_probe(guint32 host_be, guint32 source_be, int timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0) {
        return -1;
    }
    if (source_be) {
        swapp_net_bind_source(sock, source_be);
    }

    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(SWAPP_NET_PORT);
    to.sin_addr.s_addr = host_be;

    if (connect(sock, (struct sockaddr *)&to, sizeof(to)) < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }

    struct pollfd pfd = {sock, POLLOUT, 0};
    int error = 0;
    socklen_t error_len = sizeof(error);
    /* poll() reports writable for refusal as well as success, so the
     * verdict has to come from the socket error itself. */
    if (poll(&pfd, 1, timeout_ms) != 1
        || getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &error_len) < 0
        || error != 0) {
        close(sock);
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    return sock;
}

/* Sweeps the local subnets for a host accepting on SWAPP_NET_PORT. Probes
 * run in batches so a full /24 takes about a second rather than 254 serial
 * timeouts. Returns a connected socket, or -1 if nothing answered. */
#define SWAPP_SWEEP_BATCH 32
#define SWAPP_SWEEP_TIMEOUT_MS 400

/* Wherever the server was last actually found. Tried as a single connect
 * before the sweep, so the normal case costs one attempt instead of a walk
 * of the subnet. Never authoritative: if it doesn't answer, the sweep runs
 * and finds the server at whatever address it moved to, which then becomes
 * the new last-known-good. Cached on disk so a restart doesn't pay for the
 * first sweep again. */
static char g_last_good[64] = "";

static char *swapp_net_cache_path(void) {
    return g_build_filename(g_get_user_cache_dir(), "swapp", "last-server", NULL);
}

static void swapp_net_load_last_good(void) {
    char *path = swapp_net_cache_path();
    char *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        g_strlcpy(g_last_good, g_strstrip(contents), sizeof(g_last_good));
        g_free(contents);
    }
    g_free(path);
}

static void swapp_net_save_last_good(const char *addr) {
    if (g_strcmp0(g_last_good, addr) == 0) {
        return;
    }
    g_strlcpy(g_last_good, addr, sizeof(g_last_good));

    char *path = swapp_net_cache_path();
    char *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) == 0) {
        g_file_set_contents(path, addr, -1, NULL);
    }
    g_free(dir);
    g_free(path);
}

static int swapp_net_sweep(char *addr, size_t addr_size) {
    swapp_subnet subnets[8];
    int subnet_count = swapp_net_local_subnets(subnets, G_N_ELEMENTS(subnets));

    struct in_addr hint;
    if (g_last_good[0] != '\0' && inet_pton(AF_INET, g_last_good, &hint) == 1) {
        for (int s = 0; s < subnet_count; s++) {
            if ((hint.s_addr & htonl(0xffffff00u)) != subnets[s].network_be) {
                continue; /* not on this interface's subnet any more */
            }
            int sock = swapp_net_probe(hint.s_addr, subnets[s].self_be,
                                       SWAPP_SWEEP_TIMEOUT_MS);
            if (sock >= 0) {
                g_strlcpy(addr, g_last_good, addr_size);
                return sock;
            }
        }
    }

    for (int s = 0; s < subnet_count && !g_stop; s++) {
        for (int base = 1; base < 255 && !g_stop; base += SWAPP_SWEEP_BATCH) {
            int socks[SWAPP_SWEEP_BATCH];
            guint32 hosts[SWAPP_SWEEP_BATCH];
            struct pollfd pfds[SWAPP_SWEEP_BATCH];
            int pending = 0;

            for (int i = 0; i < SWAPP_SWEEP_BATCH && base + i < 255; i++) {
                guint32 host = (guint32)(base + i);
                if (host == subnets[s].self_host) {
                    continue;
                }
                guint32 host_be = subnets[s].network_be | htonl(host);

                int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
                if (sock < 0) {
                    continue;
                }
                swapp_net_bind_source(sock, subnets[s].self_be);

                struct sockaddr_in to = {0};
                to.sin_family = AF_INET;
                to.sin_port = htons(SWAPP_NET_PORT);
                to.sin_addr.s_addr = host_be;

                if (connect(sock, (struct sockaddr *)&to, sizeof(to)) < 0
                    && errno != EINPROGRESS) {
                    close(sock);
                    continue;
                }
                socks[pending] = sock;
                hosts[pending] = host_be;
                pfds[pending].fd = sock;
                pfds[pending].events = POLLOUT;
                pfds[pending].revents = 0;
                pending++;
            }

            if (pending == 0) {
                continue;
            }

            int found = -1;
            guint32 found_host = 0;
            if (poll(pfds, (nfds_t)pending, SWAPP_SWEEP_TIMEOUT_MS) > 0) {
                for (int i = 0; i < pending && found < 0; i++) {
                    if (!(pfds[i].revents & POLLOUT)) {
                        continue;
                    }
                    int error = 0;
                    socklen_t error_len = sizeof(error);
                    if (getsockopt(socks[i], SOL_SOCKET, SO_ERROR, &error, &error_len) == 0
                        && error == 0) {
                        found = i;
                        found_host = hosts[i];
                    }
                }
            }

            for (int i = 0; i < pending; i++) {
                if (i != found) {
                    close(socks[i]);
                }
            }

            if (found >= 0) {
                struct in_addr in = {found_host};
                if (!inet_ntop(AF_INET, &in, addr, (socklen_t)addr_size)) {
                    close(socks[found]);
                    return -1;
                }

                swapp_net_save_last_good(addr);

                /* Hand the socket back blocking, the mode the pump below
                 * expects. */
                int flags = fcntl(socks[found], F_GETFL, 0);
                fcntl(socks[found], F_SETFL, flags & ~O_NONBLOCK);
                return socks[found];
            }
        }
    }

    return -1;
}

/* Applies the options the command pump depends on: no Nagle delay for
 * short commands, and a bounded read so the keepalive timeout is checked
 * even on a silent socket. */
static void swapp_net_prepare(int sock) {
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct timeval timeout = {1, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

/* Pumps one connection until it dies. Nothing but keepalive travels over
 * it yet; commands land here once the server owns the monitor state. */
/* Consumes one complete line from the server. The only traffic so far is
 * the keepalive and the monitor list block; anything unrecognized is
 * ignored rather than fatal, so an older client survives a newer server
 * adding lines. Returns nonzero to keep the connection. */
static int swapp_net_handle_line(int sock, const char *line, GString *block) {
    if (strcmp(line, "ping") == 0) {
        /* Answer, so silence stays a real signal in both directions. */
        return swapp_net_send_line(sock, "pong\n");
    }
    if (strcmp(line, "pong") == 0) {
        return 1;
    }

    if (g_str_has_prefix(line, "monitors ")) {
        g_string_assign(block, "");
        return 1;
    }

    if (strcmp(line, "acquire") == 0) {
        /* Its own thread, and it does its own waiting; nothing here waits on
         * it, and a failure (no kscreen-doctor) is only logged. */
        swapp_monitors_acquire_async();
        return 1;
    }

    if (strcmp(line, "scanning") == 0) {
        /* The server has a client but no results yet. Whatever was shown
         * before is from the previous server process and no longer true. */
        g_mutex_lock(&g_state_lock);
        g_monitors[0] = '\0';
        g_monitors_ready = 0;
        g_mutex_unlock(&g_state_lock);
        g_idle_add(swapp_net_notify, NULL);
        return 1;
    }

    if (strcmp(line, "end") == 0) {
        /* The block is only published once it is whole: a half-received
         * list shown mid-transfer would flicker rows that were never
         * really there. */
        g_mutex_lock(&g_state_lock);
        g_strlcpy(g_monitors, block->str, sizeof(g_monitors));
        /* A block naming no monitors is a server that hasn't finished
         * looking, not a machine without screens. */
        g_monitors_ready = (block->len > 0);
        g_mutex_unlock(&g_state_lock);
        g_idle_add(swapp_net_notify, NULL);
        return 1;
    }

    if (g_str_has_prefix(line, "monitor ") || g_str_has_prefix(line, "input ")
        || g_str_has_prefix(line, "windows ") || g_str_has_prefix(line, "linux ")) {
        g_string_append(block, line);
        g_string_append_c(block, '\n');
    }
    return 1;
}

/* Pumps one connection until it dies. */
static void swapp_net_serve(int sock, const char *addr) {
    g_mutex_lock(&g_state_lock);
    g_sock = sock;
    g_mutex_unlock(&g_state_lock);
    swapp_net_set_state(SWAPP_NET_CONNECTED, addr);

    gint64 last_received = g_get_monotonic_time();
    gint64 last_ping = 0;
    char buffer[1024];
    GString *pending = g_string_new("");
    GString *block = g_string_new("");

    while (!g_stop) {
        gint64 now = g_get_monotonic_time();

        if (now - last_ping >= SWAPP_NET_PING_SECONDS * G_USEC_PER_SEC) {
            if (!swapp_net_send_line(sock, "ping\n")) {
                break;
            }
            last_ping = now;
        }

        ssize_t received = recv(sock, buffer, sizeof(buffer), 0);
        if (received > 0) {
            last_received = g_get_monotonic_time();
            g_string_append_len(pending, buffer, received);

            /* TCP gives no message boundaries, so lines are cut here
             * rather than assumed to arrive one per read. */
            char *newline;
            int alive = 1;
            while (alive && (newline = strchr(pending->str, '\n')) != NULL) {
                *newline = '\0';
                alive = swapp_net_handle_line(sock, g_strstrip(pending->str), block);
                g_string_erase(pending, 0, (gssize)(newline - pending->str) + 1);
            }
            if (!alive) {
                break;
            }
        } else if (received == 0) {
            break; /* clean close by the server */
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }

        if (g_get_monotonic_time() - last_received
            >= SWAPP_NET_TIMEOUT_SECONDS * (gint64)G_USEC_PER_SEC) {
            g_warning("swapp: server %s stopped responding", addr);
            break;
        }
    }

    g_string_free(pending, TRUE);
    g_string_free(block, TRUE);
    g_mutex_lock(&g_state_lock);
    g_sock = -1;
    g_mutex_unlock(&g_state_lock);
    close(sock);

    /* Back to square one: no link, no list, and the search starts again. */
    swapp_net_set_state(SWAPP_NET_WAITING, NULL);
}

static gpointer swapp_net_thread(gpointer user_data) {
    (void)user_data;

    while (!g_stop) {
        swapp_net_set_state(SWAPP_NET_WAITING, NULL);

        char addr[64];
        int sock = swapp_net_sweep(addr, sizeof(addr));
        if (sock < 0) {
            /* Nothing answered. The server may simply not be running, so
             * pause before sweeping the subnet again. */
            g_usleep(G_USEC_PER_SEC);
            continue;
        }
        swapp_net_prepare(sock);

        swapp_net_serve(sock, addr);
    }

    swapp_net_set_state(SWAPP_NET_WAITING, NULL);
    return NULL;
}

void swapp_net_start(void) {
    g_stop = 0;
    swapp_net_load_last_good();
    g_thread_new("swapp-net", swapp_net_thread, NULL);
}

void swapp_net_stop(void) {
    g_stop = 1;
}

swapp_net_state swapp_net_get_state(void) {
    g_mutex_lock(&g_state_lock);
    swapp_net_state state = g_state;
    g_mutex_unlock(&g_state_lock);
    return state;
}

void swapp_net_status_text(char *buf, size_t buf_size) {
    g_mutex_lock(&g_state_lock);
    if (g_state == SWAPP_NET_CONNECTED) {
        g_snprintf(buf, (gulong)buf_size, "Connected to %s", g_peer);
    } else {
        g_strlcpy(buf, "Searching for server...", buf_size);
    }
    g_mutex_unlock(&g_state_lock);
}

void swapp_net_set_state_callback(void (*callback)(void)) {
    g_state_callback = callback;
}

int swapp_net_monitors_ready(void) {
    g_mutex_lock(&g_state_lock);
    int ready = g_monitors_ready;
    g_mutex_unlock(&g_state_lock);
    return ready;
}

void swapp_net_monitors_text(char *buf, size_t buf_size) {
    g_mutex_lock(&g_state_lock);
    g_strlcpy(buf, g_monitors, buf_size);
    g_mutex_unlock(&g_state_lock);
}

/* Server-side entry points; the client has no cache to push and runs no
 * scans of its own. */
void swapp_net_send_monitors(void) {
}

void swapp_net_set_rescan_callback(void (*callback)(void)) {
    (void)callback;
}

void swapp_net_set_switch_callback(void (*callback)(int monitor_index, int input_code)) {
    (void)callback;
}

void swapp_net_send_acquire(void) {
}

void swapp_net_set_switch_all_callback(void (*callback)(int role)) {
    (void)callback;
}

void swapp_net_request_switch_all(int role) {
    if (role != SWAPP_ROLE_WINDOWS && role != SWAPP_ROLE_LINUX) {
        return;
    }
    g_mutex_lock(&g_state_lock);
    int sock = g_sock;
    if (sock >= 0) {
        /* As for a single switch: the server rescans after, so the UI locks
         * now rather than when `scanning` arrives. */
        g_monitors[0] = '\0';
        g_monitors_ready = 0;
    }
    g_mutex_unlock(&g_state_lock);
    if (sock < 0) {
        return;
    }
    swapp_net_send_line(sock, role == SWAPP_ROLE_WINDOWS ? "switchall windows\n"
                                                         : "switchall linux\n");
    g_idle_add(swapp_net_notify, NULL);
}

void swapp_net_set_assign_callback(void (*callback)(int monitor_index, int input_code, int role)) {
    (void)callback;
}

void swapp_net_request_assign(int monitor_index, int input_code, int role) {
    g_mutex_lock(&g_state_lock);
    int sock = g_sock;
    g_mutex_unlock(&g_state_lock);
    if (sock < 0) {
        return;
    }
    const char *name = role == SWAPP_ROLE_WINDOWS ? "windows"
                       : role == SWAPP_ROLE_LINUX ? "linux"
                                                  : "none";
    char line[64];
    g_snprintf(line, sizeof(line), "assign %d %d %s\n", monitor_index, input_code, name);
    /* Like switch: the result arrives as a fresh list from the server. */
    swapp_net_send_line(sock, line);
}

void swapp_net_request_switch(int monitor_index, int input_code) {
    g_mutex_lock(&g_state_lock);
    int sock = g_sock;
    if (sock >= 0) {
        /* The server follows every switch with a rescan; drop the list now,
         * as for a requested rescan, so the UI locks without waiting for
         * the server's `scanning`. */
        g_monitors[0] = '\0';
        g_monitors_ready = 0;
    }
    g_mutex_unlock(&g_state_lock);
    if (sock < 0) {
        return;
    }
    char line[64];
    g_snprintf(line, sizeof(line), "switch %d %d\n", monitor_index, input_code);
    /* The result comes back as a fresh monitor list; a dead link is the
     * socket thread's to notice. */
    swapp_net_send_line(sock, line);
    g_idle_add(swapp_net_notify, NULL);
}

void swapp_net_request_rescan(void) {
    g_mutex_lock(&g_state_lock);
    int sock = g_sock;
    if (sock >= 0) {
        g_monitors[0] = '\0';
        g_monitors_ready = 0;
    }
    g_mutex_unlock(&g_state_lock);
    if (sock < 0) {
        return;
    }
    /* A failed send needs no handling here: the socket thread sees the
     * same dead link on its next ping and drops it. */
    swapp_net_send_line(sock, "rescan\n");
    g_idle_add(swapp_net_notify, NULL);
}

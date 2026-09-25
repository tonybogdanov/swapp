#ifndef SWAPP_NET_H
#define SWAPP_NET_H

#include <stddef.h>

/* The link between the two halves of Swapp: the Windows server, which owns
 * the monitors, and the Linux client, which drives it. See CLAUDE.md for
 * why the split exists; this header is the contract both sides implement.
 *
 * The port is fixed (no configuration anywhere) and picked from the dynamic
 * range. There is no discovery protocol: the client sweeps its own LAN with
 * outbound TCP connects and the first host that accepts is the server.
 *
 * That is a deliberate choice over a UDP broadcast probe. The client runs on
 * a locked-down work laptop that drops every unsolicited inbound packet, and
 * a broadcast probe's reply arrives from the server's unicast address rather
 * than the broadcast address the request went to -- so connection tracking
 * doesn't match it and the firewall drops it. A TCP connect is outbound, and
 * its response is part of the same flow, so it is always allowed. Sweeping
 * also finds and connects in one step instead of two.
 *
 * Kept below 32768, outside every OS's ephemeral port range, so no other
 * app's outbound socket can be assigned it and block the bind. */
#define SWAPP_NET_PORT 21423

/* Keepalive. Silence is the only disconnect signal that works: a machine
 * that sleeps, loses its link or is switched away from never gets to send
 * a goodbye, and a TCP socket can stay open for minutes past that. Each
 * side sends a ping every SWAPP_NET_PING_SECONDS and drops the peer after
 * SWAPP_NET_TIMEOUT_SECONDS with nothing received at all. */
#define SWAPP_NET_PING_SECONDS 2
#define SWAPP_NET_TIMEOUT_SECONDS 6

/* The link state, shown in the UI on both sides. The two halves are one
 * app: with no peer neither does anything except try to find one. */
typedef enum {
    /* Client: no server found yet, still broadcasting.
     * Server: bound and listening, no client connected. */
    SWAPP_NET_WAITING = 0,
    /* Peer connected and answering pings. */
    SWAPP_NET_CONNECTED = 1,
    /* Server only: the listening socket could not be bound (another copy
     * of Swapp is already running, or the port is taken). Terminal --
     * nothing is retried, because a second server would be exactly the
     * split-brain this design exists to prevent. */
    SWAPP_NET_FAILED = 2,
} swapp_net_state;

/* Starts the link. On Windows this binds the TCP listener; on Linux it
 * sweeps the local subnets and connects to the first host that accepts. Returns immediately -- all socket work runs
 * on its own thread. A dropped connection returns to SWAPP_NET_WAITING
 * and the search starts over; only a failed bind is permanent. */
void swapp_net_start(void);

/* Stops the link and closes any socket, for shutdown. */
void swapp_net_stop(void);

/* The current state, safe to call from the UI thread. */
swapp_net_state swapp_net_get_state(void);

/* A human-readable one-liner for the current state ("Connected to
 * 192.168.1.10", "Searching for server...", "Port 21423 already in use"),
 * written into buf. */
void swapp_net_status_text(char *buf, size_t buf_size);

/* Registers a function called whenever the state changes, so the tray can
 * refresh. Invoked on the UI thread, not the socket thread. */
void swapp_net_set_state_callback(void (*callback)(void));

/* The monitor list, the first thing that crosses the link.
 *
 * The server sends it unasked -- once when its DDC/CI scan finishes, and
 * again to any client that connects after that -- as a block of lines:
 *
 *     monitors <count>
 *     monitor <index> <active-code> <label>
 *     windows <index> <code>          (optional: the server's own input)
 *     linux <index> <code>            (optional: the client's input, as assigned)
 *     input <index> <code> <name>
 *     end
 *
 * The client keeps a read-only mirror of that block and nothing else: it
 * never probes a monitor itself, so with no server connected it has no
 * monitor state at all. Both calls below are the client's side of that
 * mirror and do nothing on the server. */

/* Nonzero once a complete list has been received and not yet invalidated.
 * Cleared the moment the link drops, because a list from a server that is
 * no longer there says nothing about what the monitors are doing now. */
int swapp_net_monitors_ready(void);

/* The received list, formatted for display, or an empty string when none
 * has arrived. */
void swapp_net_monitors_text(char *buf, size_t buf_size);

/* Server only: pushes the current monitor cache to the connected client,
 * if there is one. Safe to call from the UI thread. While a scan is running
 * the cache is about to be replaced, so the client is sent `scanning`
 * instead, which makes it drop the list it is showing. */
void swapp_net_send_monitors(void);

/* Rescanning. Both sides have a button for it, but the scan itself only
 * ever runs on the server: the client's button sends
 *
 *     rescan
 *
 * and the server treats that exactly like its own button -- start the
 * DDC/CI scan, tell the client `scanning`, push the new list when it lands.
 * A request while a scan is already running is ignored. */

/* Client only: asks the server to rescan, and drops the mirrored list at
 * once so the UI clears without waiting for the server's `scanning`. Safe
 * to call from the UI thread; does nothing with no server connected. */
void swapp_net_request_rescan(void);

/* Server only: registers the function run when the client asks for a
 * rescan. Invoked from the socket thread, so it is expected to marshal to
 * the UI thread itself (PostMessage), same as the state callback. */
void swapp_net_set_rescan_callback(void (*callback)(void));

/* Switching one monitor to one input. As with rescanning, both sides offer
 * it (the table's right-click Switch) but only the server touches DDC/CI:
 * the client sends
 *
 *     switch <monitor-index> <input-code>
 *
 * with the index and code exactly as the server's list gave them, and the
 * server handles it like its own Switch: validate against its cache, write
 * VCP 0x60, then rescan -- `scanning` goes out at once, the DDC/CI queries
 * start a couple of seconds later, once the monitor has settled on the new
 * input -- and push the fresh list. A request that names no known monitor or
 * input, or arrives mid-scan, is ignored. */

/* Topology: which input of each monitor is cabled to which machine. It
 * cannot be detected (see monitors.h), so the user assigns it from either
 * side's table menu. The server owns and persists it; the client sends
 *
 *     assign <monitor-index> <input-code> windows|linux|none
 *
 * and the server applies it exactly like its own menu, then pushes the
 * updated list, whose `windows`/`linux` lines are all the client ever
 * knows about it. Invalid requests, and requests mid-scan, are ignored. */

/* Client only: asks the server to assign role (a swapp_input_role value)
 * to the input. Safe to call from the UI thread; does nothing with no server
 * connected. */
void swapp_net_request_assign(int monitor_index, int input_code, int role);

/* Server only: registers the function run when the client asks for an
 * assignment. Invoked from the socket thread; marshal to the UI thread. */
void swapp_net_set_assign_callback(void (*callback)(int monitor_index, int input_code, int role));

/* After writing a switch to an input assigned to Linux, the server also
 * sends
 *
 *     acquire
 *
 * and the client forces a modeset (swapp_monitors_acquire_async) so it
 * actually starts driving the monitor again. The server is the side that
 * knows both the switch happened and whose input it went to, and the only
 * side that can see the monitor: it sends `acquire` once the monitor answers
 * cleanly on the new input past a fixed floor (an earlier modeset leaves the
 * screen dark or garbled; see SWAPP_WATCH_READY_MIN_MS), and again if no
 * signal arrives after it. */

/* Switching everything to one machine -- the Windows / Linux buttons -- is
 * the per-cell Switch applied to every monitor's input assigned to that
 * machine. The client sends
 *
 *     switchall windows|linux
 *
 * and the server runs it like its own button, including the same checks:
 * ignored unless the server has settled (a finished scan with every
 * monitor's inputs read, nothing running) and every monitor has both a
 * Windows and a Linux input assigned. */

/* Client only: asks the server to switch every monitor to role's input
 * (SWAPP_ROLE_WINDOWS or SWAPP_ROLE_LINUX). Drops the mirrored list at once,
 * as the server rescans after. No-op with no server connected. */
void swapp_net_request_switch_all(int role);

/* Server only: registers the function run for a client's `switchall`.
 * Invoked from the socket thread; marshal to the UI thread. */
void swapp_net_set_switch_all_callback(void (*callback)(int role));

/* Server only: sends `acquire` to the connected client, if any. */
void swapp_net_send_acquire(void);

/* Client only: asks the server to switch. Safe to call from the UI thread;
 * does nothing with no server connected. */
void swapp_net_request_switch(int monitor_index, int input_code);

/* Server only: registers the function run when the client asks for a
 * switch. Invoked from the socket thread; marshal to the UI thread. */
void swapp_net_set_switch_callback(void (*callback)(int monitor_index, int input_code));

#endif

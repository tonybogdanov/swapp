#ifndef SWAPP_MONITORS_H
#define SWAPP_MONITORS_H

#include <stddef.h>

/* Enumerates connected DDC/CI-capable monitors and caches each one's
 * current input state, replacing the previous cache. Runs on a worker
 * thread and returns immediately: probing every monitor over DDC/CI takes
 * seconds, and the UI has to stay live through it so a spinner can
 * actually animate and Quit still works. Declared alongside the
 * role-driven jobs further down, which share its worker. */
void swapp_monitors_rescan_async(void);

/* Windows only. The same rescan, but the worker waits delay_ms before its
 * first DDC/CI query -- for right after a switch, when the monitor is still
 * changing inputs and a query would read the old state or fail. The job
 * counts as busy from the moment of the call, so the UI locks at once; only
 * the querying waits. An abort during the wait ends it like any job. */
void swapp_monitors_rescan_delayed_async(long delay_ms);

/* Windows only. The rescan that follows a switch to Linux. Instead of a
 * fixed delay, the worker first watches the monitors: watch_codes, indexed
 * like the cache, names the Linux input each monitor is now on (-1: not
 * watched). acquire is called on the worker thread once the watched
 * monitors have finished switching -- the cue for the client's modeset --
 * and again whenever one then goes too long without a signal, up to a fixed
 * number of times. The rescan runs once each watched monitor has a signal
 * or has been given up on. */
void swapp_monitors_rescan_watched_async(const int *watch_codes, size_t n_watch, void (*acquire)(void));

/* For every monitor in the cache, tells it to deselect its active input
 * (VCP 0x60 -> 0, the MCCS "Auto Select" sentinel) so compliant firmware
 * auto-switches to another live input rather than just going blank. Writes
 * a summary of what was sent into buf (NUL-terminated, truncated to fit
 * buf_size). */
void swapp_monitors_switch_active(char *buf, size_t buf_size);

/* Read-only views of the cache, for the monitor list window. No DDC/CI
 * traffic. Indices are valid until the next rescan. */
size_t swapp_monitors_count(void);

/* Writes a display label for monitor `index` (e.g. "Monitor 1" or, on
 * Linux, "Monitor 1 (DP-2)") into buf. */
void swapp_monitors_label(size_t index, char *buf, size_t buf_size);

/* Fills codes with the VCP 0x60 input codes monitor `index` reports as
 * supported, in capabilities order. Returns the count (0 if unknown). */
int swapp_monitors_inputs(size_t index, int *codes, int max_codes);

/* The cached active input code of monitor `index`, or -1 if unknown. */
int swapp_monitors_active_input(size_t index);

/* Re-reads just the active input (VCP 0x60) of every cached monitor,
 * skipping the connector walk and capabilities reads a full rescan does.
 * Cheap enough to run right before acting on the cache, which matters
 * because the other machine switches these same monitors without this one
 * ever hearing about it. A monitor whose read fails keeps its previous
 * cached value rather than becoming unknown. */
void swapp_monitors_refresh_active_inputs(void);

/* Maps a VESA MCCS VCP 0x60 input source code to a human-readable name
 * (e.g. 0x11 -> "HDMI-1"), or "Input-0xNN" for an unrecognized code. The
 * returned pointer is only valid until the next call for an unrecognized
 * code (it reuses a static buffer for those). */
const char *swapp_monitors_input_name(int code);

/* Sends DDC/CI Set VCP Feature 0x60 = code to monitor `index` and, on
 * success, records code as its active input in the cache. Returns nonzero
 * on success. On Linux there is no reply to confirm with, so a write that
 * reached the bus counts as success. */
int swapp_monitors_set_input(size_t index, int code);

/* Which OS the user has assigned to an input. Per monitor, at most one input
 * is Linux and at most one is Windows; everything else is None. Assignments
 * are keyed by the monitor's label and saved through the platform roles
 * store (the registry on Windows), so they survive a rescan and a restart as
 * long as the label stays the same. */
typedef enum {
    SWAPP_ROLE_NONE = 0,
    SWAPP_ROLE_LINUX = 1,
    SWAPP_ROLE_WINDOWS = 2,
} swapp_input_role;

/* The role belonging to the OS this build runs on. Swapp always switches
 * the monitors to its *own* inputs, so the machine it is triggered from is
 * the one that takes the screens over. */
#ifdef _WIN32
#define SWAPP_ROLE_SELF SWAPP_ROLE_WINDOWS
#else
#define SWAPP_ROLE_SELF SWAPP_ROLE_LINUX
#endif

swapp_input_role swapp_roles_get(const char *monitor_label, int code);

/* Assigns role to the input. Assigning Linux or Windows takes that role
 * away from whichever other input of the same monitor held it. */
void swapp_roles_set(const char *monitor_label, int code, swapp_input_role role);

/* There is deliberately no detection of which input belongs to which
 * machine. DDC/CI reports the input a monitor is *showing*, not the one the
 * asking machine is cabled to -- a monitor on the other machine's input
 * still answers over this machine's cable, reporting that other input -- so
 * any inference from it records the wrong owner. Roles come only from the
 * user assigning them, on the server. */

/* The input code assigned role on the monitor, or -1 if none is. */
int swapp_roles_code_for(const char *monitor_label, swapp_input_role role);

/* Nonzero when there is at least one cached monitor and every one of them
 * has an input assigned role. */
int swapp_roles_all_assigned(swapp_input_role role);

/* Nonzero when the configuration is complete: at least one cached monitor,
 * and every one of them has exactly one input assigned Linux and exactly
 * one assigned Windows. (Assigning a role already takes it away from
 * whichever input held it, so "assigned" and "exactly one" are the same
 * condition here.) Nothing can be switched until this holds -- a monitor
 * with no input for the OS being left behind would be stranded. */
int swapp_roles_complete(void);

/* Nonzero when there is at least one cached monitor and every one of them
 * already has its input assigned role selected, so a switch would be a
 * no-op. Reads the cache, so pair it with
 * swapp_monitors_refresh_active_inputs() before trusting it. */
int swapp_roles_all_active(swapp_input_role role);

/* Nonzero when a trigger would actually change something: the
 * configuration is complete and at least one monitor isn't on its input
 * for role yet. */
int swapp_roles_trigger_available(swapp_input_role role);

/* Switches every cached monitor to its input assigned role. Returns
 * nonzero only if every monitor had one and accepted the command. */
int swapp_roles_switch_all(swapp_input_role role);

/* Switches every cached monitor to its input assigned role, then does
 * whatever the platform needs for the source to start driving the
 * newly-selected input again, then re-enumerates so the cache matches what
 * the monitors actually did. Runs on the same worker thread as
 * swapp_monitors_rescan_async() and likewise returns immediately. */
void swapp_monitors_trigger_async(swapp_input_role role);

/* Nonzero while either job is still running. Only one runs at a time -- a
 * request made while one is in flight is dropped. The UI replaces its rows
 * with a spinner and ignores F16 while this holds. */
int swapp_monitors_busy(void);

/* Asks a running job to stop at its next checkpoint and discard what it
 * had gathered, so quitting doesn't have to wait out a full enumeration or
 * the trigger's multi-second settle delays. Permanent: no job starts after
 * it, and the job callback never fires again. */
void swapp_monitors_cancel(void);

/* Windows only. Not permanent, unlike cancel: stops the running job at its
 * next checkpoint and discards its results, and empties the cache now. The
 * job still counts as busy until its worker has actually exited, then the
 * job callback fires (trigger_failed 0) so the UI can move on. For a lost
 * client, whose scan nobody is waiting for any more. */
void swapp_monitors_abort(void);

/* Linux (client) only. Re-acquires the monitors after the server switched
 * one to this machine's input: forces a modeset through KWin's DPMS off/on
 * right away -- the server only sends `acquire` once it has seen the monitor
 * finish switching. Needed because the MST
 * hub doesn't forward the sink's hotplug, so without a modeset i915 never
 * retrains the link and the screen stays dark (the same thing a lid close
 * and open happens to fix). Runs on its own thread; no DDC/CI traffic. */
void swapp_monitors_acquire_async(void);

/* Registers a function called on the UI thread when a job finishes and its
 * results are in the cache, so an open window can rebuild itself.
 * trigger_failed is nonzero only when the job was a trigger and some
 * monitor didn't accept its switch. */
void swapp_monitors_set_job_callback(void (*callback)(int trigger_failed));

/* Starts watching for monitor connect/disconnect at the OS level and
 * re-runs swapp_monitors_rescan() a few seconds after things settle.
 * Linux only -- on Windows this is handled inline via WM_DEVICECHANGE in
 * tray_win.c, so there is nothing to start here. */
void swapp_monitors_watch_start(void);

#endif

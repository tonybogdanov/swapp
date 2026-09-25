#include "monitors.h"
#include "monitors_internal.h"

#include <windows.h>
#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWAPP_DDC_ATTEMPTS     3
#define SWAPP_SWITCH_SETTLE_MS 3000
/* A monitor that is still changing inputs answers DDC/CI but not the
 * capabilities request, for a few seconds -- one quick retry wasn't enough
 * to outlast that (seen right after a switch), so the retries are spaced to
 * cover it. */
#define SWAPP_PROBE_PASSES     4
#define SWAPP_PROBE_RETRY_MS   1500

typedef struct {
    HANDLE hphys;
    char *capabilities; /* malloc'd, may be NULL */
    int has_active_input;
    int active_input;
} swapp_monitor_state;

/* GetPhysicalMonitorsFromHMONITOR hands out handles in per-HMONITOR groups,
 * and DestroyPhysicalMonitors must be called on each group as a whole --
 * this is what makes that possible to do later, at the next rescan or on
 * shutdown, independently of which of a group's monitors made it into the
 * DDC/CI-capable cache above. */
typedef struct {
    PHYSICAL_MONITOR *physical;
    DWORD n_physical;
} swapp_physical_group;

static swapp_monitor_state *g_monitors = NULL;
static size_t g_n_monitors = 0;
static void (*g_job_callback)(int) = NULL;

/* Set from the UI thread, read by the worker between steps. Both only ever
 * flip one way, so interlocked reads/writes are enough without a lock. */
static volatile LONG g_busy = 0;
static volatile LONG g_cancel = 0;

/* Abort, unlike cancel, is not permanent: swapp_monitors_abort() bumps the
 * generation, and the job running under the previous one stops at its next
 * checkpoint and has its results thrown away. Only one job runs at a time,
 * so the running job's generation is a single global too. */
static volatile LONG g_generation = 0;
static volatile LONG g_running_generation = 0;

static int swapp_job_stopping(void) {
    return InterlockedCompareExchange(&g_cancel, 0, 0)
           || InterlockedCompareExchange(&g_running_generation, 0, 0)
                  != InterlockedCompareExchange(&g_generation, 0, 0);
}

static swapp_physical_group *g_groups = NULL;
static size_t g_n_groups = 0;

/* DDC/CI over Dxva2 flakes fairly often (a transaction that fails can just
 * succeed on retry), so every call here gets a few attempts before being
 * treated as unsupported. */
static BOOL swapp_get_vcp_retry(HANDLE hphys, BYTE vcp_code, DWORD *current, DWORD *maximum) {
    for (int attempt = 0; attempt < SWAPP_DDC_ATTEMPTS; attempt++) {
        if (GetVCPFeatureAndVCPFeatureReply(hphys, vcp_code, NULL, current, maximum)) {
            return TRUE;
        }
    }
    return FALSE;
}

static char *swapp_get_capabilities_retry(HANDLE hphys) {
    for (int attempt = 0; attempt < SWAPP_DDC_ATTEMPTS; attempt++) {
        DWORD caps_len = 0;
        if (!GetCapabilitiesStringLength(hphys, &caps_len) || caps_len == 0) {
            continue;
        }
        char *caps = (char *)malloc(caps_len);
        if (!caps) {
            return NULL;
        }
        if (CapabilitiesRequestAndCapabilitiesReply(hphys, caps, caps_len)) {
            return caps;
        }
        free(caps);
    }
    return NULL;
}

static void swapp_monitors_release_cache(void) {
    for (size_t i = 0; i < g_n_monitors; i++) {
        free(g_monitors[i].capabilities);
    }
    free(g_monitors);
    g_monitors = NULL;
    g_n_monitors = 0;

    for (size_t i = 0; i < g_n_groups; i++) {
        DestroyPhysicalMonitors(g_groups[i].n_physical, g_groups[i].physical);
        free(g_groups[i].physical);
    }
    free(g_groups);
    g_groups = NULL;
    g_n_groups = 0;
}

typedef struct {
    swapp_monitor_state *monitors;
    size_t n_monitors;
    size_t cap_monitors;
    swapp_physical_group *groups;
    size_t n_groups;
    size_t cap_groups;
} swapp_rescan_ctx;

static int swapp_rescan_ctx_push_monitor(swapp_rescan_ctx *ctx, swapp_monitor_state m) {
    if (ctx->n_monitors == ctx->cap_monitors) {
        size_t new_cap = ctx->cap_monitors ? ctx->cap_monitors * 2 : 4;
        swapp_monitor_state *bigger = (swapp_monitor_state *)realloc(ctx->monitors, new_cap * sizeof(*ctx->monitors));
        if (!bigger) {
            free(m.capabilities);
            return 0;
        }
        ctx->monitors = bigger;
        ctx->cap_monitors = new_cap;
    }
    ctx->monitors[ctx->n_monitors++] = m;
    return 1;
}

static BOOL CALLBACK swapp_rescan_enum_proc(HMONITOR hmon, HDC hdc, LPRECT rect, LPARAM lparam) {
    (void)hdc;
    (void)rect;
    swapp_rescan_ctx *ctx = (swapp_rescan_ctx *)lparam;

    DWORD n_physical = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, &n_physical) || n_physical == 0) {
        return TRUE;
    }

    PHYSICAL_MONITOR *physical = (PHYSICAL_MONITOR *)calloc(n_physical, sizeof(PHYSICAL_MONITOR));
    if (!physical) {
        return TRUE;
    }

    if (!GetPhysicalMonitorsFromHMONITOR(hmon, n_physical, physical)) {
        free(physical);
        return TRUE;
    }

    if (ctx->n_groups == ctx->cap_groups) {
        size_t new_cap = ctx->cap_groups ? ctx->cap_groups * 2 : 4;
        swapp_physical_group *bigger = (swapp_physical_group *)realloc(ctx->groups, new_cap * sizeof(*ctx->groups));
        if (!bigger) {
            DestroyPhysicalMonitors(n_physical, physical);
            free(physical);
            return TRUE;
        }
        ctx->groups = bigger;
        ctx->cap_groups = new_cap;
    }
    ctx->groups[ctx->n_groups].physical = physical;
    ctx->groups[ctx->n_groups].n_physical = n_physical;
    ctx->n_groups++;

    for (DWORD i = 0; i < n_physical; i++) {
        HANDLE hphys = physical[i].hPhysicalMonitor;

        /* Probing VCP code 0x10 (brightness) is the standard way to tell
         * whether a monitor actually answers DDC/CI -- Windows hands out a
         * physical-monitor handle for every display regardless of whether
         * the panel speaks the protocol at all. Monitors that don't answer
         * are left out of the cache entirely. */
        DWORD unused_current = 0, unused_maximum = 0;
        if (!swapp_get_vcp_retry(hphys, 0x10, &unused_current, &unused_maximum)) {
            continue;
        }

        swapp_monitor_state m = {0};
        m.hphys = hphys;
        m.capabilities = swapp_get_capabilities_retry(hphys);

        DWORD current_input = 0, max_input = 0;
        m.has_active_input = swapp_get_vcp_retry(hphys, 0x60, &current_input, &max_input);
        m.active_input = (int)(current_input & 0xFF);

        if (!swapp_rescan_ctx_push_monitor(ctx, m)) {
            break;
        }
    }

    return TRUE;
}

static void swapp_rescan_ctx_release(swapp_rescan_ctx *ctx) {
    for (size_t i = 0; i < ctx->n_monitors; i++) {
        free(ctx->monitors[i].capabilities);
    }
    free(ctx->monitors);
    for (size_t i = 0; i < ctx->n_groups; i++) {
        DestroyPhysicalMonitors(ctx->groups[i].n_physical, ctx->groups[i].physical);
        free(ctx->groups[i].physical);
    }
    free(ctx->groups);
    memset(ctx, 0, sizeof(*ctx));
}

/* Nonzero when a monitor answered DDC/CI -- so it is in the cache -- but
 * its capabilities string didn't arrive, leaving it listed with no inputs
 * and nothing to click. Observed here: a first enumeration returned one
 * monitor complete and the second with no inputs at all, and an immediate
 * manual Refresh read both correctly. */
static int swapp_rescan_ctx_incomplete(const swapp_rescan_ctx *ctx) {
    /* Finding fewer monitors than the last scan had usually means the probe
     * ran before the hardware was ready again, not that monitors went away.
     * Seen after a switch to this machine's input: Windows re-plugs that
     * display and rebuilds the desktop, and a scan in the middle of it found
     * one monitor of two, where a rescan moments later found both. Worth
     * more looks; a monitor that really was unplugged only costs the
     * retries' delay. */
    if (ctx->n_monitors < g_n_monitors || ctx->n_monitors == 0) {
        return ctx->n_monitors < g_n_monitors;
    }
    for (size_t i = 0; i < ctx->n_monitors; i++) {
        int codes[32];
        if (swapp_monitors_parse_inputs(ctx->monitors[i].capabilities, codes,
                                        (int)(sizeof(codes) / sizeof(codes[0]))) == 0) {
            return 1;
        }
    }
    return 0;
}

void swapp_monitors_switch_active(char *buf, size_t buf_size) {
    if (buf_size == 0) {
        return;
    }
    buf[0] = '\0';

    if (g_n_monitors == 0) {
        strncpy(buf, "No DDC/CI monitors detected", buf_size - 1);
        buf[buf_size - 1] = '\0';
        return;
    }

    for (size_t i = 0; i < g_n_monitors; i++) {
        swapp_monitor_state *m = &g_monitors[i];

        char line[64];
        snprintf(line, sizeof(line), "%sMonitor %zu:", i > 0 ? "\n" : "", i + 1);
        strncat(buf, line, buf_size - strlen(buf) - 1);

        swapp_monitors_format_inputs(m->capabilities, m->has_active_input, m->active_input,
                                      " (was active)", buf, buf_size);

        /* There is no standard MCCS value meaning "deselect / auto-scan" --
         * on real hardware that turned out to just be silently ignored, since
         * it isn't one of the values the monitor's own capabilities string
         * advertises for VCP 0x60. So instead switch to a specific supported
         * input other than the current one (real DDC/CI switcher tools do
         * the same): if that input has no live signal, most monitors will
         * show "no signal" and, if the monitor's own auto-input-switch OSD
         * setting is on, keep scanning from there on their own. */
        int target = m->has_active_input ? swapp_monitors_next_input(m->capabilities, m->active_input) : -1;
        if (target >= 0) {
            char suffix[64];
            snprintf(suffix, sizeof(suffix), " -> switching to %s", swapp_monitors_input_name(target));
            strncat(buf, suffix, buf_size - strlen(buf) - 1);

            for (int attempt = 0; attempt < SWAPP_DDC_ATTEMPTS; attempt++) {
                if (SetVCPFeature(m->hphys, 0x60, (DWORD)target)) {
                    break;
                }
            }
        }
    }
}

size_t swapp_monitors_count(void) {
    return g_n_monitors;
}

void swapp_monitors_label(size_t index, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "Monitor %zu", index + 1);
}

int swapp_monitors_inputs(size_t index, int *codes, int max_codes) {
    return index < g_n_monitors ? swapp_monitors_parse_inputs(g_monitors[index].capabilities, codes, max_codes) : 0;
}


int swapp_monitors_active_input(size_t index) {
    if (index >= g_n_monitors || !g_monitors[index].has_active_input) {
        return -1;
    }
    return g_monitors[index].active_input;
}

int swapp_monitors_set_input(size_t index, int code) {
    if (index >= g_n_monitors) {
        return 0;
    }
    swapp_monitor_state *m = &g_monitors[index];
    for (int attempt = 0; attempt < SWAPP_DDC_ATTEMPTS; attempt++) {
        if (SetVCPFeature(m->hphys, 0x60, (DWORD)code)) {
            m->has_active_input = 1;
            m->active_input = code;
            return 1;
        }
    }
    return 0;
}

void swapp_monitors_refresh_active_inputs(void) {
    /* The worker owns the cache while a job runs, and the UI is showing a
     * spinner rather than anything read from it. */
    if (swapp_monitors_busy()) {
        return;
    }
    for (size_t i = 0; i < g_n_monitors; i++) {
        swapp_monitor_state *m = &g_monitors[i];
        DWORD current = 0, maximum = 0;
        if (swapp_get_vcp_retry(m->hphys, 0x60, &current, &maximum)) {
            m->has_active_input = 1;
            m->active_input = (int)(current & 0xFF);
        }
    }
}

void swapp_monitors_set_job_callback(void (*callback)(int trigger_failed)) {
    g_job_callback = callback;
}

/* --- background jobs --- */

/* Sleeps in slices and gives up early once cancelled, so Quit during a
 * trigger doesn't have to wait out the settle delay. Returns 0 if it was
 * cut short. */
static int swapp_sleep_ms(long ms) {
    while (ms > 0) {
        if (swapp_job_stopping()) {
            return 0;
        }
        long slice = ms > 100 ? 100 : ms;
        Sleep((DWORD)slice);
        ms -= slice;
    }
    return !swapp_job_stopping();
}

/* Windows is wired to the monitors directly, not through the DisplayPort
 * MST hub the Linux machine sits behind, so it does see the sink's HPD when
 * an input is reselected and retrains the link on its own -- none of the
 * forced-modeset work monitors_linux.c has to do appears to be needed here.
 * The wait still is: the monitors take a moment to finish switching, and
 * re-probing before that just reads back the input they are leaving. */
static void swapp_monitors_recover_signal(void) {
    swapp_sleep_ms(SWAPP_SWITCH_SETTLE_MS);
}

/* Most monitors one switch watches for the client's signal. */
#define SWAPP_WATCH_MAX 16

typedef struct {
    swapp_input_role role; /* SWAPP_ROLE_NONE for a plain rescan */
    LONG generation;       /* g_generation when the job started */
    long delay_ms;         /* waited out on the worker before any DDC/CI */
    int watch_codes[SWAPP_WATCH_MAX]; /* see swapp_watch_switch */
    size_t n_watch;
    void (*acquire)(void); /* non-NULL: watch instead of delay */
    int trigger_failed;
    swapp_rescan_ctx probed;
} swapp_job;

/* Results are handed back through a message-only window of this module's
 * own rather than the tray's, so the monitors layer doesn't need to know
 * any window handle to post to. */
#define SWAPP_JOB_DONE_MSG (WM_APP + 64)
#define SWAPP_JOB_CLASS    "SwappMonitorJobSink"

static HWND g_job_sink = NULL;

int swapp_monitors_busy(void) {
    return InterlockedCompareExchange(&g_busy, 0, 0) != 0;
}

void swapp_monitors_cancel(void) {
    InterlockedExchange(&g_cancel, 1);
}

void swapp_monitors_abort(void) {
    InterlockedIncrement(&g_generation);
    /* The worker only ever fills its own job's context, never the cache, so
     * the cache can go right away on this (the UI) thread. */
    swapp_monitors_release_cache();
}

/* Back on the UI thread: installing the probe results here rather than in
 * the worker is what keeps the cache single-threaded. The UI shows nothing
 * but a spinner while a job runs, so it never reads the cache the worker
 * is allowed to touch. */
static void swapp_job_finished(swapp_job *job) {
    if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
        /* Quitting: throw the results away rather than swapping them into
         * a cache that is about to be torn down anyway. */
        swapp_rescan_ctx_release(&job->probed);
    } else if (job->generation != InterlockedCompareExchange(&g_generation, 0, 0)) {
        /* Aborted: the results belong to a state that no longer exists.
         * Busy is only released now that the worker has really stopped, so
         * a new job never drives the monitors alongside the old one. The
         * callback still runs, so the UI can start whatever comes next. */
        swapp_rescan_ctx_release(&job->probed);
        InterlockedExchange(&g_busy, 0);
        if (g_job_callback) {
            g_job_callback(0);
        }
    } else {
        /* Every retry came back without some monitor's capabilities: keep
         * what the previous scan read for it rather than listing it with no
         * inputs. Matched by position, since that is all there is on this
         * side, and only when the monitor count is unchanged -- a different
         * count means the old positions say nothing about the new ones.
         * Capabilities describe the panel, not its current state, so a
         * previous read stays accurate. */
        if (job->probed.n_monitors == g_n_monitors) {
            for (size_t i = 0; i < g_n_monitors; i++) {
                swapp_monitor_state *m = &job->probed.monitors[i];
                int codes[32];
                if (swapp_monitors_parse_inputs(m->capabilities, codes, 32) == 0
                    && g_monitors[i].capabilities) {
                    free(m->capabilities);
                    m->capabilities = _strdup(g_monitors[i].capabilities);
                }
            }
        }
        swapp_monitors_release_cache();
        g_monitors = job->probed.monitors;
        g_n_monitors = job->probed.n_monitors;
        g_groups = job->probed.groups;
        g_n_groups = job->probed.n_groups;
        InterlockedExchange(&g_busy, 0);
        if (g_job_callback) {
            g_job_callback(job->trigger_failed);
        }
    }
    free(job);
}

static LRESULT CALLBACK swapp_job_sink_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == SWAPP_JOB_DONE_MSG) {
        swapp_job_finished((swapp_job *)lp);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

typedef struct {
    HANDLE handles[SWAPP_WATCH_MAX];
    size_t n;
    swapp_physical_group groups[SWAPP_WATCH_MAX];
    size_t n_groups;
} swapp_watch_set;

static BOOL CALLBACK swapp_watch_enum_proc(HMONITOR hmon, HDC hdc, LPRECT rect, LPARAM lparam) {
    (void)hdc;
    (void)rect;
    swapp_watch_set *set = (swapp_watch_set *)lparam;
    DWORD n_physical = 0;
    if (set->n_groups == SWAPP_WATCH_MAX || !GetNumberOfPhysicalMonitorsFromHMONITOR(hmon, &n_physical)
        || n_physical == 0) {
        return TRUE;
    }
    PHYSICAL_MONITOR *physical = (PHYSICAL_MONITOR *)calloc(n_physical, sizeof(PHYSICAL_MONITOR));
    if (!physical) {
        return TRUE;
    }
    if (!GetPhysicalMonitorsFromHMONITOR(hmon, n_physical, physical)) {
        free(physical);
        return TRUE;
    }
    set->groups[set->n_groups].physical = physical;
    set->groups[set->n_groups].n_physical = n_physical;
    set->n_groups++;
    for (DWORD i = 0; i < n_physical && set->n < SWAPP_WATCH_MAX; i++) {
        set->handles[set->n++] = physical[i].hPhysicalMonitor;
    }
    return TRUE;
}

/* After a switch to Linux: the window in which the first acquire goes out
 * once the watched monitors are ready, how long a watched monitor may then
 * sit with no signal before the client is asked again, and how many times
 * it is asked again.
 *
 * Measured on this hardware: a monitor answers cleanly on the new input
 * ~1.2-2.1s after the switch, goes busy again (garbage answers) around
 * 3.3-4.2s, and is clean from ~4.2-5.1s on. That first clean answer is
 * misleading -- a modeset at 1.5s left one monitor showing only every other
 * line, and at the old fixed 3s some switches stayed dark -- while at 5s all
 * of them came up. The busy spell is the monitor putting up its "No
 * HDMI-2 signal" message, and a clean answer after it is the real settle
 * point, so a monitor is ready on the first clean answer after a failure
 * seen past BUSY_FROM (earlier failures are ordinary DDC/CI flakes during
 * the first clean spell), or on any clean answer past READY_MIN should the
 * busy spell be missed. Good switches locked 4-7s after the client's
 * modeset, so RETRY is past any of them. That lock takes ~12s from the
 * switch where Windows' takes 1-2s; refresh rate, the monitor's auto input
 * switch and pausing these queries after the acquire made no difference,
 * so it is the hub and the monitor. */
#define SWAPP_WATCH_BUSY_FROM_MS 3000
#define SWAPP_WATCH_READY_MIN_MS 5000
#define SWAPP_WATCH_READY_MAX_MS 8000
#define SWAPP_WATCH_RETRY_MS     20000
#define SWAPP_WATCH_RETRIES      2
/* Between sampling rounds, and between the DDC/CI calls within a round:
 * MCCS wants a gap between a reply and the next request. */
#define SWAPP_WATCH_ROUND_MS     250
#define SWAPP_WATCH_GAP_MS       50

typedef struct {
    int code;   /* the Linux input to watch for, -1 when not watched */
    int clean;  /* has answered cleanly on code at least once */
    int busy;   /* then failed past SWAPP_WATCH_BUSY_FROM_MS */
    int ready;  /* settled: see SWAPP_WATCH_READY_MIN_MS */
    int locked; /* a Timing Report with a signal came back */
} swapp_watch;

/* Tells the client when to do its modeset after a switch to Linux, instead
 * of it waiting a fixed time, and asks again if that modeset didn't take.
 *
 * Samples every physical monitor: the input it reports (VCP 0x60) and its
 * Timing Report -- the H/V frequency of the signal it is actually
 * receiving, which is what tells a monitor showing a picture from one
 * sitting on the right input with nothing locked. No signal reads as h=0
 * v=0. Handles of its own, so it never touches the cache; they are assumed
 * to come in the cache's order, which holds while every display answers
 * DDC/CI. One attempt per call: mid-switch the monitor answers garbage, and
 * a failure is itself what the settle detection reads.
 *
 * acquire is called once every watched monitor is ready (see
 * SWAPP_WATCH_READY_MIN_MS), or after SWAPP_WATCH_READY_MAX_MS regardless.
 * Sampling then goes on until every watched monitor has a signal: one that
 * doesn't get one in SWAPP_WATCH_RETRY_MS has acquire called again, up to
 * SWAPP_WATCH_RETRIES times, after which it is given up on. A watched
 * monitor that reports some other input was switched elsewhere and stops
 * being watched. Returns 0 if the job was stopped. */
static int swapp_watch_switch(const int *watch_codes, size_t n_watch, void (*acquire)(void)) {
    swapp_watch_set set = {0};
    EnumDisplayMonitors(NULL, NULL, swapp_watch_enum_proc, (LPARAM)&set);

    swapp_watch watch[SWAPP_WATCH_MAX];
    for (size_t i = 0; i < SWAPP_WATCH_MAX; i++) {
        watch[i].code = i < n_watch && i < set.n ? watch_codes[i] : -1;
        watch[i].clean = 0;
        watch[i].busy = 0;
        watch[i].ready = 0;
        watch[i].locked = 0;
    }

    ULONGLONG start = GetTickCount64();
    ULONGLONG last_acquire = 0; /* 0 until the first acquire */
    int retries = 0;
    int ok = 1;
    while (ok) {
        for (size_t i = 0; i < set.n && ok; i++) {
            swapp_watch *w = &watch[i];
            if (w->code < 0 || w->locked) {
                continue;
            }

            DWORD cur = 0, max = 0;
            int has_input = GetVCPFeatureAndVCPFeatureReply(set.handles[i], 0x60, NULL, &cur, &max) != 0;
            ok = swapp_sleep_ms(SWAPP_WATCH_GAP_MS);
            MC_TIMING_REPORT tr = {0};
            int has_timing = GetTimingReport(set.handles[i], &tr) != 0;
            if (ok) {
                ok = swapp_sleep_ms(SWAPP_WATCH_GAP_MS);
            }

            if (has_input && (int)(cur & 0xFF) != w->code) {
                w->code = -1;
            } else if (has_timing && (tr.dwHorizontalFrequencyInHZ || tr.dwVerticalFrequencyInHZ)) {
                w->locked = 1;
                w->ready = 1;
            } else if (!w->ready) {
                ULONGLONG elapsed = GetTickCount64() - start;
                if (!has_input || !has_timing) {
                    w->busy |= w->clean && elapsed >= SWAPP_WATCH_BUSY_FROM_MS;
                } else if (w->busy || elapsed >= SWAPP_WATCH_READY_MIN_MS) {
                    w->ready = 1;
                } else {
                    w->clean = 1;
                }
            }
        }

        size_t pending = 0, unready = 0;
        for (size_t i = 0; i < set.n; i++) {
            pending += watch[i].code >= 0 && !watch[i].locked;
            unready += watch[i].code >= 0 && !watch[i].ready;
        }
        ULONGLONG now = GetTickCount64();
        if (last_acquire == 0) {
            /* Every watched monitor locking with no modeset at all makes one
             * pointless, though that isn't expected behind the hub. */
            if (pending == 0 && n_watch > 0) {
                break;
            }
            if (unready == 0 || now - start >= SWAPP_WATCH_READY_MAX_MS) {
                acquire();
                last_acquire = now;
            }
        } else if (pending > 0 && now - last_acquire >= SWAPP_WATCH_RETRY_MS) {
            if (retries == SWAPP_WATCH_RETRIES) {
                break;
            }
            retries++;
            acquire();
            last_acquire = now;
        }
        if (pending == 0 && last_acquire != 0) {
            break;
        }
        if (ok) {
            ok = swapp_sleep_ms(SWAPP_WATCH_ROUND_MS);
        }
    }

    for (size_t g = 0; g < set.n_groups; g++) {
        DestroyPhysicalMonitors(set.groups[g].n_physical, set.groups[g].physical);
        free(set.groups[g].physical);
    }
    return ok;
}

static DWORD WINAPI swapp_job_thread(LPVOID param) {
    swapp_job *job = (swapp_job *)param;

    if (job->acquire && !swapp_watch_switch(job->watch_codes, job->n_watch, job->acquire)) {
        PostMessageA(g_job_sink, SWAPP_JOB_DONE_MSG, 0, (LPARAM)job);
        return 0;
    }

    /* Abortable like every other wait here; an aborted job skips straight
     * to reporting, and its (empty) results are discarded. */
    if (job->delay_ms > 0 && !swapp_sleep_ms(job->delay_ms)) {
        PostMessageA(g_job_sink, SWAPP_JOB_DONE_MSG, 0, (LPARAM)job);
        return 0;
    }

    if (job->role != SWAPP_ROLE_NONE) {
        job->trigger_failed = !swapp_roles_switch_all(job->role);
        swapp_monitors_recover_signal();
    }
    /* One automatic retry when any monitor came back without inputs,
     * rather than leaving the user to notice and hit Refresh. The results
     * of the last pass are kept whether or not it finally came out
     * complete -- a monitor listed with no inputs is still better than one
     * dropped entirely. */
    for (int pass = 0; pass < SWAPP_PROBE_PASSES && !swapp_job_stopping(); pass++) {
        if (pass > 0) {
            swapp_rescan_ctx_release(&job->probed);
            swapp_sleep_ms(SWAPP_PROBE_RETRY_MS);
        }
        EnumDisplayMonitors(NULL, NULL, swapp_rescan_enum_proc, (LPARAM)&job->probed);
        if (!swapp_rescan_ctx_incomplete(&job->probed)) {
            break;
        }
    }

    PostMessageA(g_job_sink, SWAPP_JOB_DONE_MSG, 0, (LPARAM)job);
    return 0;
}

static void swapp_job_start(swapp_input_role role, long delay_ms, const int *watch_codes, size_t n_watch,
                            void (*acquire)(void)) {
    if (InterlockedCompareExchange(&g_cancel, 0, 0) || InterlockedCompareExchange(&g_busy, 1, 0)) {
        return;
    }

    if (!g_job_sink) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = swapp_job_sink_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = SWAPP_JOB_CLASS;
        RegisterClassA(&wc);
        g_job_sink = CreateWindowA(SWAPP_JOB_CLASS, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    }

    swapp_job *job = (swapp_job *)calloc(1, sizeof(*job));
    if (!g_job_sink || !job) {
        free(job);
        InterlockedExchange(&g_busy, 0);
        return;
    }
    job->role = role;
    job->delay_ms = delay_ms;
    job->n_watch = n_watch < SWAPP_WATCH_MAX ? n_watch : SWAPP_WATCH_MAX;
    for (size_t i = 0; i < job->n_watch; i++) {
        job->watch_codes[i] = watch_codes[i];
    }
    job->acquire = acquire;
    job->generation = InterlockedCompareExchange(&g_generation, 0, 0);
    InterlockedExchange(&g_running_generation, job->generation);

    HANDLE thread = CreateThread(NULL, 0, swapp_job_thread, job, 0, NULL);
    if (!thread) {
        free(job);
        InterlockedExchange(&g_busy, 0);
        return;
    }
    /* Nothing waits on it: on Quit the process exits out from under
     * whatever it is still doing. */
    CloseHandle(thread);
}

void swapp_monitors_rescan_async(void) {
    swapp_job_start(SWAPP_ROLE_NONE, 0, NULL, 0, NULL);
}

void swapp_monitors_rescan_delayed_async(long delay_ms) {
    swapp_job_start(SWAPP_ROLE_NONE, delay_ms, NULL, 0, NULL);
}

void swapp_monitors_rescan_watched_async(const int *watch_codes, size_t n_watch, void (*acquire)(void)) {
    swapp_job_start(SWAPP_ROLE_NONE, 0, watch_codes, n_watch, acquire);
}

void swapp_monitors_trigger_async(swapp_input_role role) {
    swapp_job_start(role, 0, NULL, 0, NULL);
}

/* --- OS role persistence: HKCU\Software\Swapp\Roles\<label> --- */

#define SWAPP_ROLES_REG_KEY "Software\\Swapp\\Roles"

static int swapp_reg_read_code(HKEY key, const char *name) {
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)&value, &size) != ERROR_SUCCESS || type != REG_DWORD) {
        return -1;
    }
    return (int)value;
}

int swapp_roles_store_load(const char *monitor_label, int *linux_code, int *windows_code) {
    char path[160];
    snprintf(path, sizeof(path), SWAPP_ROLES_REG_KEY "\\%s", monitor_label);
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }
    *linux_code = swapp_reg_read_code(key, "Linux");
    *windows_code = swapp_reg_read_code(key, "Windows");
    RegCloseKey(key);
    return *linux_code >= 0 || *windows_code >= 0;
}

static void swapp_reg_write_code(HKEY key, const char *name, int code) {
    if (code < 0) {
        RegDeleteValueA(key, name);
    } else {
        DWORD value = (DWORD)code;
        RegSetValueExA(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    }
}

void swapp_roles_store_save(const char *monitor_label, int linux_code, int windows_code) {
    char path[160];
    snprintf(path, sizeof(path), SWAPP_ROLES_REG_KEY "\\%s", monitor_label);
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) {
        return;
    }
    swapp_reg_write_code(key, "Linux", linux_code);
    swapp_reg_write_code(key, "Windows", windows_code);
    RegCloseKey(key);
}

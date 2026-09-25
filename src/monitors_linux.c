#include "monitors.h"
#include "monitors_internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <glib.h>
#include <glib-unix.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SWAPP_DDC_I2C_ADDR      0x37
#define SWAPP_EDID_I2C_ADDR     0x50
#define SWAPP_EDID_BLOCK        128
#define SWAPP_DDC_ATTEMPTS      3
#define SWAPP_RESCAN_SETTLE_SEC 3
#define SWAPP_SWITCH_SETTLE_MS  3000
#define SWAPP_DPMS_GAP_MS       300
#define SWAPP_MODESET_SETTLE_MS 1500
#define SWAPP_JOB_BUDGET_SEC    240
#define SWAPP_PROBE_PASSES      2
#define SWAPP_PROBE_RETRY_MS    1500
#define SWAPP_DDC_RETRY_MS      300
#define SWAPP_TOPOLOGY_WAIT_MS  20000
#define SWAPP_TOPOLOGY_POLL_MS  250

typedef struct {
    char i2c_path[300];
    char connector[64];
    char *capabilities; /* malloc'd, may be NULL */
    int has_active_input;
    int active_input;
} swapp_monitor_state;

static swapp_monitor_state *g_monitors = NULL;
static size_t g_n_monitors = 0;
static void (*g_job_callback)(int) = NULL;

/* Set once from the UI thread, read by the worker between steps. Both are
 * only ever flipped one way, so plain atomics are enough without a lock. */
static gint g_busy = 0;
static gint g_cancel = 0;

/* Checkpoint the worker consults between DDC transactions. The budget is a
 * backstop against a monitor that never answers, not a UX timer: a first
 * enumeration legitimately takes a minute or more on a slow link, where
 * every transaction costs over a second. Only the worker reads the
 * deadline, and it is written before the thread starts. */
static gint64 g_job_deadline_us = 0;

static int swapp_job_should_stop(void) {
    return g_atomic_int_get(&g_cancel) ||
           (g_job_deadline_us != 0 && g_get_monotonic_time() > g_job_deadline_us);
}

/* --- low-level DDC/CI over i2c-dev --- */

static void swapp_ddc_wait_reply(void) {
    /* DDC/CI requires the host wait at least 40ms before reading a reply. */
    struct timespec delay = {0, 50 * 1000 * 1000};
    nanosleep(&delay, NULL);
}

/* DDC/CI failures come in bursts rather than singly -- measured against
 * hardware, for several seconds after a modeset the two MST sinks answer
 * alternately, one failing while the other succeeds. Retrying instantly
 * just spends all the attempts inside the same bad window, so a failed
 * attempt waits before the next one. */
static void swapp_ddc_retry_pause(void) {
    struct timespec delay = {0, SWAPP_DDC_RETRY_MS * 1000L * 1000L};
    nanosleep(&delay, NULL);
}

/* Sends a DDC/CI "Get VCP Feature" request for vcp_code and checks for a
 * well-formed reply -- an i2c device existing for the connector doesn't by
 * itself mean the panel implements DDC/CI, so the protocol has to actually
 * be spoken to know. On success writes the current value to *out_current
 * (when non-NULL) and returns 1. */
static int swapp_ddc_get_vcp_once(int fd, unsigned char vcp_code, int *out_current) {
    unsigned char request[5] = {0x51, 0x82, 0x01, vcp_code, 0};
    request[4] = (unsigned char)((SWAPP_DDC_I2C_ADDR << 1) ^ request[0] ^ request[1] ^ request[2] ^ request[3]);

    if (write(fd, request, sizeof(request)) != (ssize_t)sizeof(request)) {
        return 0;
    }
    swapp_ddc_wait_reply();

    unsigned char reply[11] = {0};
    if (read(fd, reply, sizeof(reply)) != (ssize_t)sizeof(reply)) {
        return 0;
    }

    unsigned char checksum = 0x50;
    for (size_t i = 0; i < sizeof(reply) - 1; i++) {
        checksum ^= reply[i];
    }
    if (reply[1] != 0x88 || reply[2] != 0x02 || reply[3] != 0x00 || checksum != reply[sizeof(reply) - 1]) {
        return 0;
    }

    if (out_current) {
        *out_current = (reply[8] << 8) | reply[9];
    }
    return 1;
}

/* DDC/CI over i2c-dev flakes fairly often (a transaction that fails can just
 * succeed on retry), so every call here gets a few attempts before being
 * treated as unsupported. */
static int swapp_ddc_get_vcp(int fd, unsigned char vcp_code, int *out_current) {
    for (int attempt = 0; attempt < SWAPP_DDC_ATTEMPTS; attempt++) {
        if (swapp_job_should_stop()) {
            return 0;
        }
        if (swapp_ddc_get_vcp_once(fd, vcp_code, out_current)) {
            return 1;
        }
        swapp_ddc_retry_pause();
    }
    return 0;
}

/* Sends a DDC/CI "Set VCP Feature" command (opcode 0x03) once. MCCS defines
 * no reply for a Set command, so success here only means the i2c write
 * itself went through, not that the monitor acted on it. */
static int swapp_ddc_set_vcp_once(int fd, unsigned char vcp_code, unsigned int value) {
    unsigned char hi = (unsigned char)((value >> 8) & 0xFF);
    unsigned char lo = (unsigned char)(value & 0xFF);
    unsigned char request[7] = {0x51, 0x84, 0x03, vcp_code, hi, lo, 0};
    unsigned char checksum = (unsigned char)(SWAPP_DDC_I2C_ADDR << 1);
    for (size_t i = 0; i < sizeof(request) - 1; i++) {
        checksum ^= request[i];
    }
    request[sizeof(request) - 1] = checksum;
    return write(fd, request, sizeof(request)) == (ssize_t)sizeof(request);
}

/* DDC/CI over i2c-dev flakes fairly often, so this gets a few attempts too,
 * same as the get-side calls. */
static void swapp_ddc_set_vcp(int fd, unsigned char vcp_code, unsigned int value) {
    for (int attempt = 0; attempt < SWAPP_DDC_ATTEMPTS; attempt++) {
        if (swapp_ddc_set_vcp_once(fd, vcp_code, value)) {
            return;
        }
    }
}

/* Sends DDC/CI Capabilities Requests (opcode 0xF3) at increasing offsets and
 * concatenates the ASCII replies (opcode 0xE3) until the display returns an
 * empty chunk, per the VESA MCCS capabilities-string protocol. Returns a
 * NUL-terminated malloc'd string, or NULL on failure -- caller frees it. */
static char *swapp_ddc_get_capabilities_once(int fd) {
    size_t cap = 256, len = 0;
    char *result = (char *)malloc(cap);
    if (!result) {
        return NULL;
    }
    result[0] = '\0';

    unsigned int offset = 0;
    for (int chunk_n = 0; chunk_n < 32; chunk_n++) { /* hard cap against a misbehaving display */
        /* Each chunk is a full DDC round trip -- over a slow MST link they
         * run well over a second apiece, so this loop is where a quit or a
         * stuck monitor has to be noticed. */
        if (swapp_job_should_stop()) {
            break;
        }
        unsigned char request[6] = {0x51, 0x83, 0xF3, (unsigned char)(offset >> 8), (unsigned char)(offset & 0xFF), 0};
        request[5] = (unsigned char)((SWAPP_DDC_I2C_ADDR << 1) ^ request[0] ^ request[1] ^ request[2] ^ request[3] ^ request[4]);

        if (write(fd, request, sizeof(request)) != (ssize_t)sizeof(request)) {
            break;
        }
        swapp_ddc_wait_reply();

        unsigned char reply[40] = {0};
        ssize_t n = read(fd, reply, sizeof(reply));
        if (n < 3) {
            break;
        }

        unsigned char payload_len = reply[1] & 0x7F; /* opcode(1) + offset(2) + data */
        if (reply[2] != 0xE3 || payload_len < 3 || (size_t)(3 + payload_len) > (size_t)n) {
            break;
        }

        size_t total_len = 3 + payload_len;
        unsigned char checksum = 0x50;
        for (size_t i = 0; i < total_len - 1; i++) {
            checksum ^= reply[i];
        }
        if (checksum != reply[total_len - 1]) {
            break;
        }

        unsigned char data_len = payload_len - 3;
        if (data_len == 0) {
            break; /* end of capabilities string */
        }

        if (len + data_len + 1 > cap) {
            cap = (len + data_len + 1) * 2;
            char *bigger = (char *)realloc(result, cap);
            if (!bigger) {
                break;
            }
            result = bigger;
        }
        memcpy(result + len, reply + 5, data_len);
        len += data_len;
        result[len] = '\0';

        offset += data_len;
    }

    if (len == 0) {
        free(result);
        return NULL;
    }
    return result;
}

static char *swapp_ddc_get_capabilities(int fd) {
    for (int attempt = 0; attempt < SWAPP_DDC_ATTEMPTS; attempt++) {
        if (swapp_job_should_stop()) {
            return NULL;
        }
        char *caps = swapp_ddc_get_capabilities_once(fd);
        if (caps) {
            return caps;
        }
        swapp_ddc_retry_pause();
    }
    return NULL;
}

/* --- capabilities memo --- */

/* A monitor's capabilities string never changes, and reading it is by far
 * the most expensive part of an enumeration: on the DisplayPort MST hub
 * this was developed against it arrives in 26-byte chunks at roughly 1.4s
 * per DDC transaction -- 17 chunks, some 23 seconds, per monitor. Paying
 * that again on every rescan made Refresh look like a hang, so it is read
 * once and remembered for the life of the process.
 *
 * Keyed by the connector's EDID, which comes from sysfs and costs no i2c
 * traffic, and which identifies the physical panel down to its serial
 * number -- two identical monitors differ only there, so the whole block
 * has to match, the same reasoning R9 applies to MST adapter pairing. */
typedef struct {
    unsigned char edid[SWAPP_EDID_BLOCK];
    char *capabilities;
} swapp_caps_memo;

static swapp_caps_memo *g_caps_memo = NULL;
static size_t g_n_caps_memo = 0;

static const char *swapp_caps_memo_get(const unsigned char *edid) {
    for (size_t i = 0; i < g_n_caps_memo; i++) {
        if (memcmp(g_caps_memo[i].edid, edid, SWAPP_EDID_BLOCK) == 0) {
            return g_caps_memo[i].capabilities;
        }
    }
    return NULL;
}

static void swapp_caps_memo_put(const unsigned char *edid, const char *capabilities) {
    if (!capabilities || swapp_caps_memo_get(edid)) {
        return;
    }
    swapp_caps_memo *bigger =
        (swapp_caps_memo *)realloc(g_caps_memo, (g_n_caps_memo + 1) * sizeof(*g_caps_memo));
    if (!bigger) {
        return;
    }
    g_caps_memo = bigger;
    char *copy = strdup(capabilities);
    if (!copy) {
        return;
    }
    memcpy(g_caps_memo[g_n_caps_memo].edid, edid, SWAPP_EDID_BLOCK);
    g_caps_memo[g_n_caps_memo].capabilities = copy;
    g_n_caps_memo++;
}

/* Reads the first 128-byte EDID block the kernel already parsed for a
 * connector. Only used to pair an MST connector with its i2c adapter. */
static int swapp_read_connector_edid(const char *connector, unsigned char *out) {
    char path[300];
    snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", connector);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    ssize_t n = read(fd, out, SWAPP_EDID_BLOCK);
    close(fd);
    return n == SWAPP_EDID_BLOCK;
}

/* Reads the first 128-byte EDID block straight off an i2c bus, from the
 * standard EDID EEPROM at slave 0x50, offset 0.
 *
 * This has to be one combined I2C_RDWR transaction (write the offset, repeated
 * START, read) rather than a write() followed by a read(). Over DisplayPort
 * MST the two calls become two separate transactions and the remote sink does
 * not hold the offset across the STOP in between, so the read comes back with
 * whatever the EEPROM's own pointer happened to be sitting on -- verified on
 * hardware, where the two-step form returned an extension block and random
 * bytes rather than the EDID header. */
static int swapp_read_i2c_edid(const char *dev_path, unsigned char *out) {
    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        return 0;
    }

    unsigned char offset = 0;
    struct i2c_msg msgs[2] = {
        {.addr = SWAPP_EDID_I2C_ADDR, .flags = 0, .len = 1, .buf = &offset},
        {.addr = SWAPP_EDID_I2C_ADDR, .flags = I2C_M_RD, .len = SWAPP_EDID_BLOCK, .buf = out},
    };
    struct i2c_rdwr_ioctl_data xfer = {.msgs = msgs, .nmsgs = 2};

    int ok = ioctl(fd, I2C_RDWR, &xfer) >= 0;
    close(fd);
    return ok;
}

/* i915 (and other DRM drivers) expose each DisplayPort MST sink's DDC channel
 * as a standalone i2c adapter named "DPMST", parented to the GPU rather than
 * linked into the connector's sysfs directory -- so a connector behind an MST
 * hub has no `ddc` symlink at all and the normal lookup finds nothing.
 *
 * Every such adapter carries the identical name, so the only reliable way to
 * tell which one belongs to `connector` is to read the EDID off each and
 * compare it against the block the kernel already parsed for that connector.
 * Two identical monitors differ only in the serial number, so the whole
 * 128-byte block has to match, not just the model name.
 *
 * The scan is deliberately limited to DPMST-named adapters: slave 0x50 is the
 * EDID address on a display bus, but on an SMBus it is a DIMM's SPD EEPROM,
 * so probing arbitrary i2c buses for an EDID is not safe. */
static int swapp_ddc_mst_i2c_path_for_connector(const char *connector, char *path, size_t path_size) {
    unsigned char want[SWAPP_EDID_BLOCK];
    if (!swapp_read_connector_edid(connector, want)) {
        return 0;
    }

    DIR *dir = opendir("/sys/class/i2c-dev");
    if (!dir) {
        return 0;
    }

    int found = 0;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "i2c-", 4) != 0) {
            continue;
        }

        char name_path[300];
        snprintf(name_path, sizeof(name_path), "/sys/class/i2c-dev/%s/name", entry->d_name);

        FILE *f = fopen(name_path, "r");
        if (!f) {
            continue;
        }
        char name[64] = {0};
        int has_name = fgets(name, sizeof(name), f) != NULL;
        fclose(f);
        if (!has_name || strstr(name, "DPMST") == NULL) {
            continue;
        }

        char dev_path[300];
        snprintf(dev_path, sizeof(dev_path), "/dev/%s", entry->d_name);

        unsigned char got[SWAPP_EDID_BLOCK];
        if (swapp_read_i2c_edid(dev_path, got) && memcmp(got, want, SWAPP_EDID_BLOCK) == 0) {
            snprintf(path, path_size, "%s", dev_path);
            found = 1;
        }
    }
    closedir(dir);
    return found;
}

/* Resolves the connector's ddc symlink (e.g. /sys/class/drm/card1-DP-1/ddc)
 * to the /dev/i2c-N node that carries its DDC/CI channel, falling back to the
 * DPMST-by-EDID scan above for connectors that have no such symlink. */
static int swapp_ddc_i2c_path_for_connector(const char *connector, char *path, size_t path_size) {
    char link_path[300];
    snprintf(link_path, sizeof(link_path), "/sys/class/drm/%s/ddc", connector);

    char target[300];
    ssize_t len = readlink(link_path, target, sizeof(target) - 1);
    if (len < 0) {
        /* No ddc symlink -- on DisplayPort MST the adapter is not under the
         * connector, so fall back to matching it up by EDID. */
        return swapp_ddc_mst_i2c_path_for_connector(connector, path, path_size);
    }
    target[len] = '\0';

    const char *name = strrchr(target, '/');
    name = name ? name + 1 : target;
    snprintf(path, path_size, "/dev/%s", name);
    return 1;
}

/* Walks every connected DRM connector's status file. Callback returns
 * non-zero to keep going. Shared between the rescan and the hotplug watch's
 * initial inotify_add_watch pass. */
static void swapp_for_each_connected_connector(void (*cb)(const char *connector, void *ctx), void *ctx) {
    DIR *dir = opendir("/sys/class/drm");
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Connector directories are named "card<N>-<connector>", e.g. card1-DP-1. */
        if (strncmp(entry->d_name, "card", 4) != 0 || strchr(entry->d_name, '-') == NULL) {
            continue;
        }

        char status_path[300];
        snprintf(status_path, sizeof(status_path), "/sys/class/drm/%s/status", entry->d_name);

        FILE *f = fopen(status_path, "r");
        if (!f) {
            continue;
        }
        char status[16] = {0};
        int has_status = fgets(status, sizeof(status), f) != NULL;
        fclose(f);
        if (!has_status || strncmp(status, "connected", 9) != 0) {
            continue;
        }

        cb(entry->d_name, ctx);
    }
    closedir(dir);
}

/* --- cache management --- */

static void swapp_monitors_release_cache(void) {
    for (size_t i = 0; i < g_n_monitors; i++) {
        free(g_monitors[i].capabilities);
    }
    free(g_monitors);
    g_monitors = NULL;
    g_n_monitors = 0;
}

typedef struct {
    swapp_monitor_state *monitors;
    size_t n;
    size_t cap;
} swapp_rescan_ctx;

static void swapp_rescan_probe_connector(const char *connector, void *ctx_ptr) {
    swapp_rescan_ctx *ctx = (swapp_rescan_ctx *)ctx_ptr;

    if (swapp_job_should_stop()) {
        return;
    }

    char i2c_path[300];
    if (!swapp_ddc_i2c_path_for_connector(connector, i2c_path, sizeof(i2c_path))) {
        return;
    }

    int fd = open(i2c_path, O_RDWR);
    if (fd < 0) {
        return;
    }
    if (ioctl(fd, I2C_SLAVE, SWAPP_DDC_I2C_ADDR) < 0) {
        close(fd);
        return;
    }

    /* Skip monitors that don't answer DDC/CI at all rather than caching
     * them with nothing useful to say. */
    if (!swapp_ddc_get_vcp(fd, 0x10, NULL)) {
        close(fd);
        return;
    }

    swapp_monitor_state m = {0};
    strncpy(m.i2c_path, i2c_path, sizeof(m.i2c_path) - 1);
    strncpy(m.connector, connector, sizeof(m.connector) - 1);

    /* Read once per physical panel, then remembered -- see the memo above
     * for why re-reading it on every rescan is not affordable. */
    unsigned char edid[SWAPP_EDID_BLOCK];
    int have_edid = swapp_read_connector_edid(connector, edid);
    const char *remembered = have_edid ? swapp_caps_memo_get(edid) : NULL;
    if (remembered) {
        m.capabilities = strdup(remembered);
    } else {
        m.capabilities = swapp_ddc_get_capabilities(fd);
        if (have_edid) {
            swapp_caps_memo_put(edid, m.capabilities);
        }
    }

    int current_input = 0;
    m.has_active_input = swapp_ddc_get_vcp(fd, 0x60, &current_input);
    m.active_input = current_input & 0xFF;

    close(fd);

    if (ctx->n == ctx->cap) {
        size_t new_cap = ctx->cap ? ctx->cap * 2 : 4;
        swapp_monitor_state *bigger = (swapp_monitor_state *)realloc(ctx->monitors, new_cap * sizeof(*ctx->monitors));
        if (!bigger) {
            free(m.capabilities);
            return;
        }
        ctx->monitors = bigger;
        ctx->cap = new_cap;
    }
    ctx->monitors[ctx->n++] = m;
}

static void swapp_rescan_ctx_release(swapp_rescan_ctx *ctx) {
    for (size_t i = 0; i < ctx->n; i++) {
        free(ctx->monitors[i].capabilities);
    }
    free(ctx->monitors);
    ctx->monitors = NULL;
    ctx->n = ctx->cap = 0;
}

/* Nonzero when a monitor answered DDC/CI -- so it is in the cache -- but
 * its capabilities string didn't arrive, leaving it listed with no inputs
 * and nothing to click. Seen on real hardware: a monitor that comes up
 * empty on one enumeration reads fine on the very next one. */
static int swapp_rescan_ctx_incomplete(const swapp_rescan_ctx *ctx) {
    /* Finding nothing at all right after having had monitors means the
     * probe ran before the hardware was ready again, not that the monitors
     * went away -- worth one more look. */
    if (ctx->n == 0) {
        return g_n_monitors > 0;
    }
    for (size_t i = 0; i < ctx->n; i++) {
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

        char line[96];
        snprintf(line, sizeof(line), "%sMonitor %zu (%s):", i > 0 ? "\n" : "", i + 1, m->connector);
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

            int fd = open(m->i2c_path, O_RDWR);
            if (fd >= 0) {
                if (ioctl(fd, I2C_SLAVE, SWAPP_DDC_I2C_ADDR) == 0) {
                    swapp_ddc_set_vcp(fd, 0x60, (unsigned int)target);
                }
                close(fd);
            }
        }
    }
}

size_t swapp_monitors_count(void) {
    return g_n_monitors;
}

void swapp_monitors_label(size_t index, char *buf, size_t buf_size) {
    snprintf(buf, buf_size, "Monitor %zu (%s)", index + 1, index < g_n_monitors ? g_monitors[index].connector : "?");
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
    int fd = open(m->i2c_path, O_RDWR);
    if (fd < 0) {
        return 0;
    }
    int ok = ioctl(fd, I2C_SLAVE, SWAPP_DDC_I2C_ADDR) == 0;
    if (ok) {
        swapp_ddc_set_vcp(fd, 0x60, (unsigned int)code);
    }
    close(fd);
    if (ok) {
        m->has_active_input = 1;
        m->active_input = code;
    }
    return ok;
}

void swapp_monitors_refresh_active_inputs(void) {
    /* The worker owns the cache while a job runs, and the UI is showing a
     * spinner rather than anything read from it. */
    if (swapp_monitors_busy()) {
        return;
    }
    for (size_t i = 0; i < g_n_monitors; i++) {
        swapp_monitor_state *m = &g_monitors[i];
        int fd = open(m->i2c_path, O_RDWR);
        if (fd < 0) {
            continue;
        }
        int current = 0;
        if (ioctl(fd, I2C_SLAVE, SWAPP_DDC_I2C_ADDR) == 0 && swapp_ddc_get_vcp(fd, 0x60, &current)) {
            m->has_active_input = 1;
            m->active_input = current & 0xFF;
        }
        close(fd);
    }
}

void swapp_monitors_set_job_callback(void (*callback)(int trigger_failed)) {
    g_job_callback = callback;
}

/* --- post-switch signal recovery --- */

/* Sleeps in slices and gives up early once cancelled, so Quit during a
 * trigger doesn't have to wait out several seconds of settle delay.
 * Returns 0 if it was cut short. */
static int swapp_sleep_ms(long ms) {
    while (ms > 0) {
        if (g_atomic_int_get(&g_cancel)) {
            return 0;
        }
        long slice = ms > 100 ? 100 : ms;
        struct timespec delay = {0, slice * 1000L * 1000L};
        nanosleep(&delay, NULL);
        ms -= slice;
    }
    return !g_atomic_int_get(&g_cancel);
}

static void swapp_count_connector_cb(const char *connector, void *ctx) {
    (void)connector;
    (*(int *)ctx)++;
}

static int swapp_count_connected_connectors(void) {
    int n = 0;
    swapp_for_each_connected_connector(swapp_count_connector_cb, &n);
    return n;
}

/* Walks the DPMST i2c adapters, optionally checking each one answers
 * DDC/CI. Counting alone turned out to be useless as a readiness test: a
 * modeset leaves the adapters and connectors in place throughout, so the
 * counts never dip and a wait on them returns immediately. What actually
 * takes several seconds to come back is the sinks answering, so that is
 * what gets probed -- one attempt each, to keep a poll cheap. */
static int swapp_dpmst_adapters(int probe_them) {
    DIR *dir = opendir("/sys/class/i2c-dev");
    if (!dir) {
        return 0;
    }
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "i2c-", 4) != 0) {
            continue;
        }
        char path[300];
        snprintf(path, sizeof(path), "/sys/class/i2c-dev/%s/name", entry->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        char name[64] = {0};
        int is_mst = fgets(name, sizeof(name), f) && strncmp(name, "DPMST", 5) == 0;
        fclose(f);
        if (!is_mst) {
            continue;
        }
        if (!probe_them) {
            n++;
            continue;
        }

        char dev_path[300];
        snprintf(dev_path, sizeof(dev_path), "/dev/%s", entry->d_name);
        int fd = open(dev_path, O_RDWR);
        if (fd < 0) {
            continue;
        }
        if (ioctl(fd, I2C_SLAVE, SWAPP_DDC_I2C_ADDR) == 0 && swapp_ddc_get_vcp_once(fd, 0x10, NULL)) {
            n++;
        }
        close(fd);
    }
    closedir(dir);
    return n;
}

static void swapp_kscreen_dpms(const char *tool, const char *state) {
    gchar *argv[] = {(gchar *)tool, (gchar *)"--dpms", (gchar *)state, NULL};
    GError *error = NULL;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                      NULL, NULL, NULL, &error)) {
        g_warning("swapp: kscreen-doctor --dpms %s failed: %s", state, error->message);
        g_error_free(error);
    }
}

/* A monitor that is switched away from an input and later switched back to
 * it never tells the source: the DisplayPort MST hub in the chain doesn't
 * forward the sink's HPD, so the connector's sysfs status never changes,
 * i915 never retrains the link, and the compositor goes on scanning out
 * into a receiver that isn't locked on -- the screen just stays dark.
 * Restarting link training needs a modeset, and under Wayland only the
 * compositor can perform one, so this drives KWin's DPMS off/on via
 * kscreen-doctor. There is no portable equivalent; on a desktop without
 * kscreen-doctor the switch still happens and the modeset is skipped. */
static void swapp_monitors_recover_signal(void) {
    gchar *tool = g_find_program_in_path("kscreen-doctor");
    if (!tool) {
        g_warning("swapp: kscreen-doctor not found; skipping the post-switch modeset");
        return;
    }

    /* The monitors need a moment to finish selecting the new input -- a
     * modeset sent before that trains against the one they're leaving.
     * Each wait doubles as a cancellation point. */
    if (swapp_sleep_ms(SWAPP_SWITCH_SETTLE_MS)) {
        /* Taken while the topology is still up, to know what to wait for
         * once the modeset has torn it down. */
        int want_connectors = swapp_count_connected_connectors();
        int want_adapters = swapp_dpmst_adapters(0);

        swapp_kscreen_dpms(tool, "off");
        /* Long enough for KWin to commit the disable: sent back to back,
         * the two requests coalesce and nothing is actually torn down. */
        swapp_sleep_ms(SWAPP_DPMS_GAP_MS);
        /* Unconditional: having turned the outputs off, leaving them that
         * way because Quit arrived mid-cycle would hand the user two dark
         * screens and no app to fix them with. */
        swapp_kscreen_dpms(tool, "on");

        /* A modeset leaves the sinks unable to answer DDC/CI for several
         * seconds -- measured against hardware, the two MST sinks answer
         * alternately and only both settle around 4s. The enumeration
         * treats a monitor that doesn't answer as absent, so probing
         * during that window dropped them all and a Trigger reported "No
         * DDC/CI monitors detected". So wait for them to answer, bounded
         * in case they never do. */
        gint64 deadline = g_get_monotonic_time() + (gint64)SWAPP_TOPOLOGY_WAIT_MS * 1000;
        while (g_get_monotonic_time() < deadline) {
            if (swapp_count_connected_connectors() >= want_connectors &&
                swapp_dpmst_adapters(1) >= want_adapters) {
                break;
            }
            if (!swapp_sleep_ms(SWAPP_TOPOLOGY_POLL_MS)) {
                break;
            }
        }
        /* One answer each is not proof they are steady yet. */
        swapp_sleep_ms(SWAPP_MODESET_SETTLE_MS);
    }

    g_free(tool);
}

/* --- acquire: the client's half of a switch to its input --- */

static gint g_acquire_running = 0;

/* The same DPMS cycle as swapp_monitors_recover_signal(), minus the settle
 * wait and the DDC/CI readiness polling: on the client the server owns the
 * monitors and is the one querying them -- it only sends `acquire` once it
 * has seen them settle -- so this only restarts link training and returns. */
static gpointer swapp_acquire_thread(gpointer user_data) {
    (void)user_data;
    gchar *tool = g_find_program_in_path("kscreen-doctor");
    if (!tool) {
        g_warning("swapp: kscreen-doctor not found; cannot re-acquire the monitors");
    } else {
        swapp_kscreen_dpms(tool, "off");
        swapp_sleep_ms(SWAPP_DPMS_GAP_MS);
        /* Unconditional once "off" went out -- see recover_signal. */
        swapp_kscreen_dpms(tool, "on");
        g_free(tool);
    }
    g_atomic_int_set(&g_acquire_running, 0);
    return NULL;
}

void swapp_monitors_acquire_async(void) {
    /* A second request while one cycle runs is covered by that cycle. */
    if (!g_atomic_int_compare_and_exchange(&g_acquire_running, 0, 1)) {
        return;
    }
    GThread *thread = g_thread_try_new("swapp-acquire", swapp_acquire_thread, NULL, NULL);
    if (!thread) {
        g_atomic_int_set(&g_acquire_running, 0);
        return;
    }
    g_thread_unref(thread);
}

/* --- background jobs --- */

typedef struct {
    swapp_input_role role; /* SWAPP_ROLE_NONE for a plain rescan */
    int trigger_failed;
    swapp_rescan_ctx probed;
} swapp_job;

int swapp_monitors_busy(void) {
    return g_atomic_int_get(&g_busy);
}

void swapp_monitors_cancel(void) {
    g_atomic_int_set(&g_cancel, 1);
}

/* Back on the UI thread: installing the probe results here rather than in
 * the worker is what keeps the cache single-threaded. The UI shows nothing
 * but a spinner while a job runs, so it never reads the cache the worker
 * is allowed to touch. */
static gboolean swapp_job_finished(gpointer user_data) {
    swapp_job *job = (swapp_job *)user_data;

    if (g_atomic_int_get(&g_cancel)) {
        /* Quitting: throw the results away rather than swapping them into
         * a cache that is about to be torn down anyway. */
        swapp_rescan_ctx_release(&job->probed);
    } else {
        swapp_monitors_release_cache();
        g_monitors = job->probed.monitors;
        g_n_monitors = job->probed.n;
        /* Disarmed so the cheap UI-thread reads outside a job don't see a
         * deadline that expired during one. */
        g_job_deadline_us = 0;
        g_atomic_int_set(&g_busy, 0);
        if (g_job_callback) {
            g_job_callback(job->trigger_failed);
        }
    }

    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer swapp_job_thread(gpointer user_data) {
    swapp_job *job = (swapp_job *)user_data;

    if (job->role != SWAPP_ROLE_NONE) {
        job->trigger_failed = !swapp_roles_switch_all(job->role);
        swapp_monitors_recover_signal();
    }
    /* One automatic retry when any monitor came back without inputs,
     * rather than leaving the user to notice and hit Refresh. The whole
     * pass is redone from scratch, but the capabilities memo means only
     * the monitor that actually failed is re-read over DDC -- the ones
     * that succeeded are already remembered. The results of the last pass
     * are kept whether or not it finally came out complete. */
    for (int pass = 0; pass < SWAPP_PROBE_PASSES && !swapp_job_should_stop(); pass++) {
        if (pass > 0) {
            swapp_rescan_ctx_release(&job->probed);
            swapp_sleep_ms(SWAPP_PROBE_RETRY_MS);
        }
        swapp_for_each_connected_connector(swapp_rescan_probe_connector, &job->probed);
        if (!swapp_rescan_ctx_incomplete(&job->probed)) {
            break;
        }
    }

    g_idle_add(swapp_job_finished, job);
    return NULL;
}

static void swapp_job_start(swapp_input_role role) {
    if (g_atomic_int_get(&g_cancel) || !g_atomic_int_compare_and_exchange(&g_busy, 0, 1)) {
        return;
    }
    g_job_deadline_us = g_get_monotonic_time() + (gint64)SWAPP_JOB_BUDGET_SEC * G_USEC_PER_SEC;

    swapp_job *job = g_new0(swapp_job, 1);
    job->role = role;
    /* Unreferenced rather than joined: nothing waits on it, and on Quit the
     * process exits out from under whatever it is still doing. */
    g_thread_unref(g_thread_new("swapp-monitors", swapp_job_thread, job));
}

void swapp_monitors_rescan_async(void) {
    swapp_job_start(SWAPP_ROLE_NONE);
}

void swapp_monitors_trigger_async(swapp_input_role role) {
    swapp_job_start(role);
}

/* --- OS-level hotplug watch --- */

static guint g_rescan_timeout_id = 0;

static gboolean swapp_rescan_timeout(gpointer user_data) {
    (void)user_data;
    /* Dropped if a job is already running -- that job ends in a fresh
     * enumeration of its own, so the hotplug is picked up regardless. */
    swapp_monitors_rescan_async();
    g_rescan_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

/* Debounces bursts of connector-status events (a single physical
 * plug/unplug can touch several connectors' status files in quick
 * succession) into one rescan a few seconds after the last of them. */
static void swapp_schedule_rescan(void) {
    if (g_rescan_timeout_id) {
        g_source_remove(g_rescan_timeout_id);
    }
    g_rescan_timeout_id = g_timeout_add_seconds(SWAPP_RESCAN_SETTLE_SEC, swapp_rescan_timeout, NULL);
}

static gboolean swapp_inotify_ready(gint fd, GIOCondition condition, gpointer user_data) {
    (void)condition;
    (void)user_data;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    if (read(fd, buf, sizeof(buf)) > 0) {
        swapp_schedule_rescan();
    }
    return G_SOURCE_CONTINUE;
}

void swapp_monitors_watch_start(void) {
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) {
        g_warning("swapp: inotify_init1 failed; monitor hotplug detection disabled");
        return;
    }

    /* Connector directories persist across cable plug/unplug on the usual
     * (non-hot-pluggable-GPU) case -- only their status file's content
     * changes. Watch every connector, not just the currently-connected
     * ones, so a future connect on a port that's empty right now is still
     * caught. */
    DIR *dir = opendir("/sys/class/drm");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "card", 4) != 0 || strchr(entry->d_name, '-') == NULL) {
                continue;
            }
            char status_path[300];
            snprintf(status_path, sizeof(status_path), "/sys/class/drm/%s/status", entry->d_name);
            inotify_add_watch(fd, status_path, IN_MODIFY | IN_CLOSE_WRITE);
        }
        closedir(dir);
    }

    g_unix_fd_add(fd, G_IO_IN, swapp_inotify_ready, NULL);
}

/* --- OS role persistence: $XDG_CONFIG_HOME/swapp/roles.ini --- */

static gchar *swapp_roles_ini_path(void) {
    return g_build_filename(g_get_user_config_dir(), "swapp", "roles.ini", NULL);
}

static int swapp_keyfile_code(GKeyFile *kf, const char *group, const char *key) {
    GError *err = NULL;
    gint value = g_key_file_get_integer(kf, group, key, &err);
    if (err) {
        g_error_free(err);
        return -1;
    }
    return value;
}

int swapp_roles_store_load(const char *monitor_label, int *linux_code, int *windows_code) {
    gchar *path = swapp_roles_ini_path();
    GKeyFile *kf = g_key_file_new();
    int found = 0;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        *linux_code = swapp_keyfile_code(kf, monitor_label, "Linux");
        *windows_code = swapp_keyfile_code(kf, monitor_label, "Windows");
        found = *linux_code >= 0 || *windows_code >= 0;
    }
    g_key_file_free(kf);
    g_free(path);
    return found;
}

void swapp_roles_store_save(const char *monitor_label, int linux_code, int windows_code) {
    gchar *path = swapp_roles_ini_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);

    if (linux_code >= 0) {
        g_key_file_set_integer(kf, monitor_label, "Linux", linux_code);
    } else {
        g_key_file_remove_key(kf, monitor_label, "Linux", NULL);
    }
    if (windows_code >= 0) {
        g_key_file_set_integer(kf, monitor_label, "Windows", windows_code);
    } else {
        g_key_file_remove_key(kf, monitor_label, "Windows", NULL);
    }

    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(dir);
    g_key_file_free(kf);
    g_free(path);
}

#include "monitors.h"
#include "monitors_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VESA MCCS input source codes (VCP 0x60 value list). */
const char *swapp_monitors_input_name(int code) {
    switch (code) {
        case 0x01: return "VGA-1";
        case 0x02: return "VGA-2";
        case 0x03: return "DVI-1";
        case 0x04: return "DVI-2";
        case 0x05: return "Composite-1";
        case 0x06: return "Composite-2";
        case 0x07: return "S-Video-1";
        case 0x08: return "S-Video-2";
        case 0x09: return "Tuner-1";
        case 0x0A: return "Tuner-2";
        case 0x0B: return "Tuner-3";
        case 0x0C: return "Component-1";
        case 0x0D: return "Component-2";
        case 0x0E: return "Component-3";
        case 0x0F: return "DisplayPort-1";
        case 0x10: return "DisplayPort-2";
        case 0x11: return "HDMI-1";
        case 0x12: return "HDMI-2";
        default: {
            static char buf[16];
            snprintf(buf, sizeof(buf), "Input-0x%02X", code);
            return buf;
        }
    }
}

/* Finds the value-list block for VCP code `code` (e.g. "60") inside a DDC/CI
 * capabilities string, i.e. the text right after "60(" in "...vcp(02 04 ...
 * 60(0F 11 12) 62 ...)...". Requires the code not be preceded by a hex digit,
 * so it can't match the tail of a longer token. */
static const char *swapp_find_vcp_block(const char *caps, const char *code) {
    size_t code_len = strlen(code);
    const char *p = caps;
    while ((p = strstr(p, code)) != NULL) {
        int prev_ok = (p == caps) || !isxdigit((unsigned char)p[-1]);
        if (prev_ok && p[code_len] == '(') {
            return p + code_len + 1;
        }
        p += 1;
    }
    return NULL;
}

void swapp_monitors_format_inputs(const char *capabilities, int has_current, int current_value,
                                   const char *marker, char *out, size_t out_size) {
    const char *block = capabilities ? swapp_find_vcp_block(capabilities, "60") : NULL;
    if (!block) {
        strncat(out, " (inputs unknown)", out_size - strlen(out) - 1);
        return;
    }

    const char *p = block;
    int first = 1;
    while (*p != '\0' && *p != ')') {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0' || *p == ')') {
            break;
        }

        char *end;
        long code = strtol(p, &end, 16);
        if (end == p) {
            break;
        }
        p = end;

        /* Some monitor firmwares echo the single-byte input code into both
         * bytes of the (nominally 16-bit) current value -- e.g. HDMI-1
         * (0x11) comes back as 0x1111 -- so compare against the low byte. */
        int is_current = has_current && code == (current_value & 0xFF);
        char item[48];
        snprintf(item, sizeof(item), "%s%s%s", first ? " " : ", ", swapp_monitors_input_name((int)code),
                 is_current ? marker : "");
        strncat(out, item, out_size - strlen(out) - 1);
        first = 0;
    }

    if (first) {
        strncat(out, " (no inputs listed)", out_size - strlen(out) - 1);
    }
}

int swapp_monitors_parse_inputs(const char *capabilities, int *codes, int max_codes) {
    const char *block = capabilities ? swapp_find_vcp_block(capabilities, "60") : NULL;
    if (!block) {
        return 0;
    }

    int n = 0;
    const char *p = block;
    while (*p != '\0' && *p != ')' && n < max_codes) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0' || *p == ')') {
            break;
        }
        char *end;
        long code = strtol(p, &end, 16);
        if (end == p) {
            break;
        }
        codes[n++] = (int)code;
        p = end;
    }
    return n;
}

int swapp_monitors_next_input(const char *capabilities, int current_value) {
    int codes[32];
    int n = swapp_monitors_parse_inputs(capabilities, codes, (int)(sizeof(codes) / sizeof(codes[0])));
    if (n < 2) {
        return -1;
    }

    int current = current_value & 0xFF;
    for (int i = 0; i < n; i++) {
        if (codes[i] == current) {
            return codes[(i + 1) % n];
        }
    }
    /* Current input isn't in its own capabilities list (seen on some
     * firmwares) -- fall back to the first listed input. */
    return codes[0];
}

/* --- OS role assignments --- */

#define SWAPP_ROLES_MAX_MONITORS 16

typedef struct {
    char label[96];
    int linux_code;   /* -1 when unassigned */
    int windows_code; /* -1 when unassigned */
} swapp_roles_entry;

static swapp_roles_entry g_roles[SWAPP_ROLES_MAX_MONITORS];
static size_t g_n_roles = 0;

static swapp_roles_entry *swapp_roles_find(const char *label, int create) {
    for (size_t i = 0; i < g_n_roles; i++) {
        if (strcmp(g_roles[i].label, label) == 0) {
            return &g_roles[i];
        }
    }
    if (g_n_roles == SWAPP_ROLES_MAX_MONITORS) {
        return NULL;
    }
    /* Not seen yet this run: pull any saved assignment in from the
     * platform store before deciding whether an entry is needed. */
    int linux_code = -1, windows_code = -1;
    int saved = swapp_roles_store_load(label, &linux_code, &windows_code);
    if (!create && !saved) {
        return NULL;
    }
    swapp_roles_entry *e = &g_roles[g_n_roles++];
    strncpy(e->label, label, sizeof(e->label) - 1);
    e->label[sizeof(e->label) - 1] = '\0';
    e->linux_code = linux_code;
    e->windows_code = windows_code;
    return e;
}

swapp_input_role swapp_roles_get(const char *monitor_label, int code) {
    swapp_roles_entry *e = swapp_roles_find(monitor_label, 0);
    if (!e) {
        return SWAPP_ROLE_NONE;
    }
    if (e->linux_code == code) {
        return SWAPP_ROLE_LINUX;
    }
    if (e->windows_code == code) {
        return SWAPP_ROLE_WINDOWS;
    }
    return SWAPP_ROLE_NONE;
}

int swapp_roles_code_for(const char *monitor_label, swapp_input_role role) {
    swapp_roles_entry *e = swapp_roles_find(monitor_label, 0);
    if (!e) {
        return -1;
    }
    return role == SWAPP_ROLE_LINUX ? e->linux_code : role == SWAPP_ROLE_WINDOWS ? e->windows_code : -1;
}

int swapp_roles_all_assigned(swapp_input_role role) {
    size_t n = swapp_monitors_count();
    if (n == 0) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char label[96];
        swapp_monitors_label(i, label, sizeof(label));
        if (swapp_roles_code_for(label, role) < 0) {
            return 0;
        }
    }
    return 1;
}

int swapp_roles_all_active(swapp_input_role role) {
    size_t n = swapp_monitors_count();
    if (n == 0) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char label[96];
        swapp_monitors_label(i, label, sizeof(label));
        int code = swapp_roles_code_for(label, role);
        if (code < 0 || swapp_monitors_active_input(i) != code) {
            return 0;
        }
    }
    return 1;
}

int swapp_roles_complete(void) {
    return swapp_roles_all_assigned(SWAPP_ROLE_LINUX) && swapp_roles_all_assigned(SWAPP_ROLE_WINDOWS);
}

int swapp_roles_trigger_available(swapp_input_role role) {
    return swapp_roles_complete() && !swapp_roles_all_active(role);
}

int swapp_roles_switch_all(swapp_input_role role) {
    int ok = 1;
    size_t n = swapp_monitors_count();
    for (size_t i = 0; i < n; i++) {
        char label[96];
        swapp_monitors_label(i, label, sizeof(label));
        int code = swapp_roles_code_for(label, role);
        if (code < 0 || !swapp_monitors_set_input(i, code)) {
            ok = 0;
        }
    }
    return ok;
}

void swapp_roles_set(const char *monitor_label, int code, swapp_input_role role) {
    swapp_roles_entry *e = swapp_roles_find(monitor_label, role != SWAPP_ROLE_NONE);
    if (!e) {
        return;
    }
    /* Clear whatever this input held, then take the new role over from any
     * other input that had it. */
    if (e->linux_code == code) {
        e->linux_code = -1;
    }
    if (e->windows_code == code) {
        e->windows_code = -1;
    }
    if (role == SWAPP_ROLE_LINUX) {
        e->linux_code = code;
    } else if (role == SWAPP_ROLE_WINDOWS) {
        e->windows_code = code;
    }
    swapp_roles_store_save(e->label, e->linux_code, e->windows_code);
}

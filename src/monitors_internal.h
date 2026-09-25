#ifndef SWAPP_MONITORS_INTERNAL_H
#define SWAPP_MONITORS_INTERNAL_H

#include <stddef.h>

/* Parses the VCP 0x60 (Input Source Select) value list out of a DDC/CI
 * capabilities string into codes (at most max_codes). Returns the count, 0
 * if the list is missing or unparseable. */
int swapp_monitors_parse_inputs(const char *capabilities, int *codes, int max_codes);

/* Parses the VCP 0x60 (Input Source Select) value list out of a DDC/CI
 * capabilities string and appends it to out as a comma-separated list of
 * input names. The entry matching current_value (when has_current) gets
 * `marker` appended right after its name (e.g. "*" or " (off)"). */
void swapp_monitors_format_inputs(const char *capabilities, int has_current, int current_value,
                                   const char *marker, char *out, size_t out_size);

/* Returns the input code that comes right after current_value in the
 * capabilities string's VCP 0x60 value list, wrapping around. Returns -1 if
 * the list has fewer than two entries or can't be parsed; if current_value
 * isn't itself in the list, returns the list's first entry. */
int swapp_monitors_next_input(const char *capabilities, int current_value);

/* Platform persistence for OS role assignments (Windows: registry under
 * HKCU\Software\Swapp\Roles; Linux: $XDG_CONFIG_HOME/swapp/roles.ini).
 * Codes are -1 when unassigned. load returns nonzero if anything was saved
 * for the label. */
int swapp_roles_store_load(const char *monitor_label, int *linux_code, int *windows_code);
void swapp_roles_store_save(const char *monitor_label, int linux_code, int windows_code);

#endif

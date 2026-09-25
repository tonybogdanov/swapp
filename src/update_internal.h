#ifndef SWAPP_UPDATE_INTERNAL_H
#define SWAPP_UPDATE_INTERNAL_H

#include <stddef.h>

/* Per-OS HTTPS GET of `url` into `buf` (NUL-terminated). Fails, returning
 * zero, on anything but a complete 200 response that fits. */
int swapp_http_get_text(const char *url, const char *accept, char *buf, size_t buf_size);

/* The tag of the release found by the last successful
 * swapp_update_fetch_latest, e.g. "build-9". Downloads use it, so the binary
 * always comes from the release whose hash was just compared. */
const char *swapp_update_tag(void);

#endif

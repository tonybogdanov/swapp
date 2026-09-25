#ifndef SWAPP_ASSETS_H
#define SWAPP_ASSETS_H

#include <stddef.h>

/* Bundled fonts and icons are compiled into the binary (see
 * cmake/embed_assets.cmake), so a release is a single file with nothing to
 * unpack next to it. */
typedef struct {
    const char *name; /* path under assets/, '/'-separated, e.g. "icons/check.png" */
    const unsigned char *data;
    size_t size;
} swapp_asset;

extern const swapp_asset swapp_assets[];
extern const size_t swapp_asset_count;

/* Looks up `name` (e.g. "icons/check.png"). Returns NULL if it isn't bundled. */
const swapp_asset *swapp_asset_find(const char *name);

#endif

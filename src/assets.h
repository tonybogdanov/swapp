#ifndef SWAPP_ASSETS_H
#define SWAPP_ASSETS_H

#include <stddef.h>

/* Bundled fonts and icons live next to the executable, not at a path fixed
 * at build time: the app is run straight out of its build directory, so
 * resolving relative to the running binary is the only thing that works in
 * both that case and an installed one.
 *
 * Writes the absolute path of `relative` (e.g. "icons/check.png") into buf.
 * Returns nonzero on success. */
int swapp_asset_path(const char *relative, char *buf, size_t buf_size);

#endif

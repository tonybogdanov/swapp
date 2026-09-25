#ifndef SWAPP_UPDATE_H
#define SWAPP_UPDATE_H

#include <stddef.h>

/* The build's short commit hash, baked in by CMake. There is no semver: a
 * release is just the latest main, so the hash is the version. */
#ifndef SWAPP_COMMIT
#define SWAPP_COMMIT "unknown"
#endif

#define SWAPP_REPO_URL "https://github.com/tonybogdanov/swapp"

/* The single release always sits at "latest", so fixed download URLs work.
 * version.txt is published beside the binaries and holds the hash of the
 * commit they were built from -- one plain download, no API or JSON. */
#define SWAPP_RELEASE_URL SWAPP_REPO_URL "/releases/latest/download/"

/* Passed to a downloaded binary so the installed copy it hands over to
 * knows to delete it (see install.h). */
#define SWAPP_ARG_UPDATE "--update"
#define SWAPP_ARG_CLEANUP "--cleanup"

/* All of these block; call them off the UI thread. */

/* Fetches the latest release's commit hash into `hash`. Nonzero on success. */
int swapp_update_fetch_latest(char *hash, size_t hash_size);

/* Called from the downloading thread; `total` is 0 when unknown. */
typedef void (*swapp_update_progress_fn)(unsigned long long done, unsigned long long total,
                                         void *ctx);

/* Downloads this OS's release binary to a temporary file, whose path is
 * written to `path` (UTF-8). Nonzero on success. */
int swapp_update_download(char *path, size_t path_size, swapp_update_progress_fn progress,
                          void *ctx);

/* Starts the downloaded binary with SWAPP_ARG_UPDATE. It installs itself
 * over this one, which includes asking this running instance to quit. */
int swapp_update_launch(const char *path);

#endif

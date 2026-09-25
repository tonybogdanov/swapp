#include "install.h"
#include "tray.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int swapp_install_redirect(void) {
    char self[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length <= 0) {
        return 0;
    }
    self[length] = '\0';

    char *dir = g_build_filename(g_get_home_dir(), ".local", "bin", NULL);
    char *target = g_build_filename(dir, "swapp", NULL);
    gchar *contents = NULL;
    gsize size = 0;

    /* /proc/self/exe is fully resolved, so resolve the target the same way
     * before comparing (~/.local/bin may itself be a symlink). */
    char resolved[PATH_MAX];
    if (realpath(target, resolved) && strcmp(resolved, self) == 0) {
        goto done; /* already the installed copy */
    }

    if (!g_file_get_contents(self, &contents, &size, NULL)) {
        goto done;
    }

    /* A different binary is an update. The running copy is stopped first so
     * the relaunch below starts the new one rather than just finding the old
     * one and asking it to show its window. g_file_set_contents writes a
     * temp file and renames it over the target, so a binary still mapped by
     * a process is never written in place. */
    gchar *installed = NULL;
    gsize installed_size = 0;
    int current = g_file_get_contents(target, &installed, &installed_size, NULL)
                  && installed_size == size && memcmp(installed, contents, size) == 0;
    g_free(installed);
    if (!current) {
        swapp_tray_stop_running();
        if (g_mkdir_with_parents(dir, 0755) != 0
            || !g_file_set_contents(target, contents, (gssize)size, NULL)
            || g_chmod(target, 0755) != 0) {
            goto done;
        }
    }

    /* Replacing this process rather than spawning a child: same effect as
     * exiting and starting the installed copy, with nothing left behind. */
    char *const argv[] = {target, NULL};
    execv(target, argv);
    /* Only reached if execv failed: run from here instead. */

done:
    g_free(contents);
    g_free(target);
    g_free(dir);
    return 0;
}

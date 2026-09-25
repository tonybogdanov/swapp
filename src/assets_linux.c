#include "assets.h"

#include <glib.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

int swapp_asset_path(const char *relative, char *buf, size_t buf_size) {
    char exe[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length <= 0) {
        return 0;
    }
    exe[length] = '\0';

    char *dir = g_path_get_dirname(exe);
    char *path = g_build_filename(dir, "assets", relative, NULL);
    g_strlcpy(buf, path, buf_size);
    g_free(path);
    g_free(dir);
    return 1;
}

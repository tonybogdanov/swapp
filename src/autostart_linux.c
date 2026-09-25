#include "autostart.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <unistd.h>

static char *swapp_autostart_path(void) {
    return g_build_filename(g_get_user_config_dir(), "autostart", "swapp.desktop", NULL);
}

int swapp_autostart_enabled(void) {
    char *path = swapp_autostart_path();
    int exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return exists;
}

int swapp_autostart_set(int enabled) {
    char *path = swapp_autostart_path();
    int ok = 0;

    if (!enabled) {
        ok = g_unlink(path) == 0 || !g_file_test(path, G_FILE_TEST_EXISTS);
        g_free(path);
        return ok;
    }

    char self[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length > 0) {
        self[length] = '\0';
        char *dir = g_path_get_dirname(path);
        /* Quoted per the Desktop Entry spec, in case the path has spaces. */
        char *entry = g_strdup_printf("[Desktop Entry]\n"
                                      "Type=Application\n"
                                      "Name=Swapp\n"
                                      "Exec=\"%s\"\n"
                                      "X-GNOME-Autostart-enabled=true\n",
                                      self);
        ok = g_mkdir_with_parents(dir, 0700) == 0 && g_file_set_contents(path, entry, -1, NULL);
        g_free(entry);
        g_free(dir);
    }
    g_free(path);
    return ok;
}

#include "assets.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

int swapp_asset_path(const char *relative, char *buf, size_t buf_size) {
    char exe[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (length == 0 || length >= sizeof(exe)) {
        return 0;
    }

    char *slash = strrchr(exe, '\\');
    if (!slash) {
        return 0;
    }
    *slash = '\0';

    _snprintf_s(buf, buf_size, _TRUNCATE, "%s\\assets\\%s", exe, relative);
    return 1;
}

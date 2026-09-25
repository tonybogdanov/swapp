#include "autostart.h"

#include <windows.h>
#include <wchar.h>

#define SWAPP_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define SWAPP_RUN_VALUE L"Swapp"

int swapp_autostart_enabled(void) {
    return RegGetValueW(HKEY_CURRENT_USER, SWAPP_RUN_KEY, SWAPP_RUN_VALUE, RRF_RT_REG_SZ, NULL,
                        NULL, NULL) == ERROR_SUCCESS;
}

int swapp_autostart_set(int enabled) {
    if (!enabled) {
        LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, SWAPP_RUN_KEY, SWAPP_RUN_VALUE);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }

    WCHAR self[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, self, ARRAYSIZE(self));
    if (length == 0 || length >= ARRAYSIZE(self)) {
        return 0;
    }
    WCHAR command[MAX_PATH + 2];
    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE, L"\"%s\"", self);
    return RegSetKeyValueW(HKEY_CURRENT_USER, SWAPP_RUN_KEY, SWAPP_RUN_VALUE, REG_SZ, command,
                           (DWORD)((wcslen(command) + 1) * sizeof(WCHAR))) == ERROR_SUCCESS;
}

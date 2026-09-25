#include "install.h"
#include "tray.h"

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* Byte-for-byte comparison; nonzero when both files exist and match. */
static int swapp_files_equal(const WCHAR *a, const WCHAR *b) {
    FILE *fa = NULL;
    FILE *fb = NULL;
    int equal = 0;
    if (_wfopen_s(&fa, a, L"rb") == 0 && _wfopen_s(&fb, b, L"rb") == 0) {
        char ba[65536];
        char bb[65536];
        for (;;) {
            size_t na = fread(ba, 1, sizeof(ba), fa);
            size_t nb = fread(bb, 1, sizeof(bb), fb);
            if (na != nb || memcmp(ba, bb, na) != 0) {
                break;
            }
            if (na == 0) {
                equal = 1;
                break;
            }
        }
    }
    if (fa) {
        fclose(fa);
    }
    if (fb) {
        fclose(fb);
    }
    return equal;
}

int swapp_install_redirect(void) {
    WCHAR self[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, self, ARRAYSIZE(self));
    if (length == 0 || length >= ARRAYSIZE(self)) {
        return 0;
    }

    PWSTR programs = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_UserProgramFiles, KF_FLAG_CREATE, NULL, &programs))) {
        return 0;
    }
    WCHAR dir[MAX_PATH];
    WCHAR target[MAX_PATH];
    _snwprintf_s(dir, ARRAYSIZE(dir), _TRUNCATE, L"%s\\swapp", programs);
    _snwprintf_s(target, ARRAYSIZE(target), _TRUNCATE, L"%s\\swapp.exe", dir);
    CoTaskMemFree(programs);

    if (_wcsicmp(self, target) == 0) {
        return 0; /* already the installed copy */
    }

    /* A different binary is an update: the installed copy, if running, is
     * locked by Windows, so it is stopped before being overwritten. */
    if (!swapp_files_equal(self, target)) {
        swapp_tray_stop_running();
        CreateDirectoryW(dir, NULL);
        if (!CopyFileW(self, target, FALSE)) {
            return 0;
        }
    }

    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(target, NULL, NULL, NULL, FALSE, 0, NULL, dir, &startup, &process)) {
        return 0;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}

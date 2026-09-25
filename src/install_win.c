#include "install.h"
#include "tray.h"
#include "update.h"

#include <windows.h>
#include <shellapi.h>
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

/* Deletes the download that handed over to this copy. The downloader exits
 * right after starting this process, but until it has, Windows keeps its
 * image locked, so this retries for a few seconds. */
static void swapp_install_cleanup(const WCHAR *path) {
    for (int i = 0; i < 50; i++) {
        if (DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND) {
            return;
        }
        Sleep(200);
    }
}

static int swapp_install_redirect_wide(const WCHAR *cleanup, int from_update) {
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
        if (cleanup) {
            swapp_install_cleanup(cleanup);
        }
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

    WCHAR command[MAX_PATH * 2 + 32];
    if (from_update) {
        _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
                     L"\"%s\" " L"" SWAPP_ARG_CLEANUP L" \"%s\"", target, self);
    } else {
        _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE, L"\"%s\"", target);
    }

    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(target, command, NULL, NULL, FALSE, 0, NULL, dir, &startup, &process)) {
        return 0;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}

int swapp_install_redirect(int argc, char **argv) {
    /* argv is in the ANSI code page; the wide command line keeps any path. */
    (void)argc;
    (void)argv;
    int wargc = 0;
    WCHAR **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    const WCHAR *cleanup = NULL;
    int from_update = 0;
    for (int i = 1; wargv && i < wargc; i++) {
        if (wcscmp(wargv[i], L"" SWAPP_ARG_UPDATE) == 0) {
            from_update = 1;
        } else if (wcscmp(wargv[i], L"" SWAPP_ARG_CLEANUP) == 0 && i + 1 < wargc) {
            cleanup = wargv[++i];
        }
    }
    int redirected = swapp_install_redirect_wide(cleanup, from_update);
    LocalFree(wargv);
    return redirected;
}

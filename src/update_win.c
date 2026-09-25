#include "update.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

typedef int (*swapp_http_sink)(const void *data, DWORD size, void *ctx);

/* GETs `url` over HTTPS, following redirects (GitHub's release links bounce
 * to a CDN host; WinHTTP follows HTTPS->HTTPS by default), and streams the
 * body to `sink`. Nonzero on a complete 200 response. */
static int swapp_http_get(const WCHAR *url, swapp_http_sink sink, void *sink_ctx,
                          swapp_update_progress_fn progress, void *progress_ctx) {
    WCHAR host[256];
    WCHAR path[2048];
    URL_COMPONENTS parts = {sizeof(parts)};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url, 0, 0, &parts)) {
        return 0;
    }

    int ok = 0;
    HINTERNET session = WinHttpOpen(L"swapp", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = session ? WinHttpConnect(session, host, parts.nPort, 0) : NULL;
    HINTERNET request = connection
        ? WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
        : NULL;
    if (!request
        || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                               0, 0, 0)
        || !WinHttpReceiveResponse(request, NULL)) {
        goto done;
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)
        || status != 200) {
        goto done;
    }
    DWORD total = 0;
    size = sizeof(total);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &total, &size, WINHTTP_NO_HEADER_INDEX);

    unsigned long long done_bytes = 0;
    char buffer[65536];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read)) {
            goto done;
        }
        if (read == 0) {
            break;
        }
        if (!sink(buffer, read, sink_ctx)) {
            goto done;
        }
        done_bytes += read;
        if (progress) {
            progress(done_bytes, total, progress_ctx);
        }
    }
    ok = 1;

done:
    if (request) {
        WinHttpCloseHandle(request);
    }
    if (connection) {
        WinHttpCloseHandle(connection);
    }
    if (session) {
        WinHttpCloseHandle(session);
    }
    return ok;
}

typedef struct {
    char *buf;
    size_t size;
    size_t used;
} swapp_mem_sink;

static int swapp_sink_memory(const void *data, DWORD size, void *ctx) {
    swapp_mem_sink *mem = ctx;
    if (mem->used + size >= mem->size) {
        return 0; /* version.txt is a few bytes; anything bigger is wrong */
    }
    memcpy(mem->buf + mem->used, data, size);
    mem->used += size;
    mem->buf[mem->used] = '\0';
    return 1;
}

static int swapp_sink_file(const void *data, DWORD size, void *ctx) {
    return fwrite(data, 1, size, (FILE *)ctx) == size;
}

int swapp_update_fetch_latest(char *hash, size_t hash_size) {
    char body[64] = {0};
    swapp_mem_sink mem = {body, sizeof(body), 0};
    if (!swapp_http_get(L"" SWAPP_RELEASE_URL "version.txt", swapp_sink_memory, &mem, NULL, NULL)) {
        return 0;
    }
    size_t length = strspn(body, "0123456789abcdef");
    if (length == 0 || length >= hash_size) {
        return 0;
    }
    memcpy(hash, body, length);
    hash[length] = '\0';
    return 1;
}

int swapp_update_download(char *path, size_t path_size, swapp_update_progress_fn progress,
                          void *ctx) {
    WCHAR temp[MAX_PATH];
    WCHAR file[MAX_PATH];
    DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    if (length == 0 || length >= ARRAYSIZE(temp)) {
        return 0;
    }
    _snwprintf_s(file, ARRAYSIZE(file), _TRUNCATE, L"%sswapp-update.exe", temp);

    FILE *out = NULL;
    if (_wfopen_s(&out, file, L"wb") != 0) {
        return 0;
    }
    int ok = swapp_http_get(L"" SWAPP_RELEASE_URL "swapp.exe", swapp_sink_file, out, progress, ctx);
    ok = fclose(out) == 0 && ok;
    if (!ok) {
        DeleteFileW(file);
        return 0;
    }
    return WideCharToMultiByte(CP_UTF8, 0, file, -1, path, (int)path_size, NULL, NULL) > 0;
}

int swapp_update_launch(const char *path) {
    WCHAR file[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, file, ARRAYSIZE(file)) == 0) {
        return 0;
    }
    WCHAR command[MAX_PATH + 32];
    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE, L"\"%s\" " L"" SWAPP_ARG_UPDATE, file);

    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(file, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        return 0;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 1;
}

void swapp_update_discard(const char *path) {
    WCHAR file[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, file, ARRAYSIZE(file)) != 0) {
        DeleteFileW(file);
    }
}

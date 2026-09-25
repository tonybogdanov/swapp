#include "update.h"
#include "update_internal.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>

typedef int (*swapp_http_sink)(const void *data, DWORD size, void *ctx);

/* GETs `url` (UTF-8) over HTTPS, following redirects (GitHub's release
 * links bounce to a CDN host; WinHTTP follows HTTPS->HTTPS by default), and
 * streams the body to `sink`. `accept` is an optional Accept header value.
 * Nonzero on a complete 200 response. */
static int swapp_http_get(const char *url_utf8, const char *accept, swapp_http_sink sink,
                          void *sink_ctx, swapp_update_progress_fn progress, void *progress_ctx) {
    WCHAR url[2048];
    WCHAR headers[256] = L"";
    if (MultiByteToWideChar(CP_UTF8, 0, url_utf8, -1, url, ARRAYSIZE(url)) == 0) {
        return 0;
    }
    if (accept) {
        _snwprintf_s(headers, ARRAYSIZE(headers), _TRUNCATE, L"Accept: %hs", accept);
    }
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
        || !WinHttpSendRequest(request, accept ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                               accept ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
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
        return 0; /* bigger than the caller allowed for */
    }
    memcpy(mem->buf + mem->used, data, size);
    mem->used += size;
    mem->buf[mem->used] = '\0';
    return 1;
}

static int swapp_sink_file(const void *data, DWORD size, void *ctx) {
    return fwrite(data, 1, size, (FILE *)ctx) == size;
}

int swapp_http_get_text(const char *url, const char *accept, char *buf, size_t buf_size) {
    if (buf_size == 0) {
        return 0;
    }
    buf[0] = '\0';
    swapp_mem_sink mem = {buf, buf_size, 0};
    return swapp_http_get(url, accept, swapp_sink_memory, &mem, NULL, NULL);
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
    char url[256];
    _snprintf_s(url, sizeof(url), _TRUNCATE, SWAPP_REPO_URL "/releases/download/%s/swapp.exe",
                swapp_update_tag());
    int ok = swapp_http_get(url, NULL, swapp_sink_file, out, progress, ctx);
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

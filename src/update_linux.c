#include "update.h"
#include "update_internal.h"

#include <curl/curl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    swapp_update_progress_fn fn;
    void *ctx;
} swapp_curl_progress;

static int swapp_curl_on_progress(void *user, curl_off_t total, curl_off_t done, curl_off_t ul_total,
                                  curl_off_t ul_done) {
    (void)ul_total;
    (void)ul_done;
    swapp_curl_progress *progress = user;
    if (progress->fn && done > 0) {
        progress->fn((unsigned long long)done, (unsigned long long)total, progress->ctx);
    }
    return 0;
}

typedef struct {
    char *buf;
    size_t size;
    size_t used;
} swapp_mem_sink;

static size_t swapp_curl_to_memory(char *data, size_t size, size_t count, void *user) {
    swapp_mem_sink *mem = user;
    size_t bytes = size * count;
    if (mem->used + bytes >= mem->size) {
        return 0; /* bigger than the caller allowed for */
    }
    memcpy(mem->buf + mem->used, data, bytes);
    mem->used += bytes;
    mem->buf[mem->used] = '\0';
    return bytes;
}

static gpointer swapp_curl_init_once(gpointer unused) {
    (void)unused;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return NULL;
}

/* GETs `url`, following redirects (GitHub's release links bounce to a CDN
 * host). `accept` is an optional Accept header value. Nonzero on a complete
 * 2xx response. */
static int swapp_http_get(const char *url, const char *accept, curl_write_callback write,
                          void *write_ctx, swapp_update_progress_fn progress, void *progress_ctx) {
    static GOnce once = G_ONCE_INIT;
    g_once(&once, swapp_curl_init_once, NULL);

    CURL *curl = curl_easy_init();
    if (!curl) {
        return 0;
    }
    struct curl_slist *headers = NULL;
    if (accept) {
        char *header = g_strdup_printf("Accept: %s", accept);
        headers = curl_slist_append(headers, header);
        g_free(header);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    swapp_curl_progress state = {progress, progress_ctx};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "swapp");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); /* runs on a worker thread */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    if (write) { /* NULL: libcurl's default, fwrite to the FILE * in write_ctx */
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, write_ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, swapp_curl_on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    int ok = curl_easy_perform(curl) == CURLE_OK;
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return ok;
}

int swapp_http_get_text(const char *url, const char *accept, char *buf, size_t buf_size) {
    if (buf_size == 0) {
        return 0;
    }
    buf[0] = '\0';
    swapp_mem_sink mem = {buf, buf_size, 0};
    return swapp_http_get(url, accept, swapp_curl_to_memory, &mem, NULL, NULL);
}

int swapp_update_download(char *path, size_t path_size, swapp_update_progress_fn progress,
                          void *ctx) {
    /* The cache dir rather than /tmp: the download is executed, and /tmp is
     * often mounted noexec on locked-down machines. */
    char *dir = g_build_filename(g_get_user_cache_dir(), "swapp", NULL);
    char *file = g_build_filename(dir, "swapp-update", NULL);
    int ok = 0;

    FILE *out = g_mkdir_with_parents(dir, 0700) == 0 ? fopen(file, "wb") : NULL;
    if (out) {
        char *url = g_strdup_printf(SWAPP_REPO_URL "/releases/download/%s/swapp",
                                    swapp_update_tag());
        ok = swapp_http_get(url, NULL, NULL, out, progress, ctx);
        g_free(url);
        ok = fclose(out) == 0 && ok && g_chmod(file, 0755) == 0;
        if (ok) {
            ok = g_strlcpy(path, file, path_size) < path_size;
        } else {
            g_unlink(file);
        }
    }

    g_free(file);
    g_free(dir);
    return ok;
}

int swapp_update_launch(const char *path) {
    char *argv[] = {(char *)path, SWAPP_ARG_UPDATE, NULL};
    return g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
}

void swapp_update_discard(const char *path) {
    g_unlink(path);
}

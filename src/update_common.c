#include "update.h"
#include "update_internal.h"

#include <stdio.h>
#include <string.h>

/* Written and read on the update worker threads only, one at a time. */
static char g_tag[64];

const char *swapp_update_tag(void) {
    return g_tag;
}

/* Pulls the "tag_name" string out of the release JSON. A full parser isn't
 * needed for one flat field, and tags are plain ASCII (build-<n>). */
static int swapp_parse_tag(const char *json, char *tag, size_t tag_size) {
    const char *p = strstr(json, "\"tag_name\"");
    if (!p) {
        return 0;
    }
    p += strlen("\"tag_name\"");
    p += strspn(p, " \t\r\n");
    if (*p++ != ':') {
        return 0;
    }
    p += strspn(p, " \t\r\n");
    if (*p++ != '"') {
        return 0;
    }
    size_t length = strspn(p, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-");
    if (length == 0 || length >= tag_size || p[length] != '"') {
        return 0;
    }
    memcpy(tag, p, length);
    tag[length] = '\0';
    return 1;
}

int swapp_update_fetch_latest(char *hash, size_t hash_size) {
    /* The API rather than /releases/latest/download/: for minutes after a
     * publish, some of github.com's frontends still redirect "latest" to
     * the previous release, while the API is at most a minute behind. The
     * response carries every asset's details, hence the roomy buffer. */
    static char json[128 * 1024];
    char tag[sizeof(g_tag)];
    if (!swapp_http_get_text(SWAPP_API_URL "/releases/latest", "application/vnd.github+json",
                             json, sizeof(json))
        || !swapp_parse_tag(json, tag, sizeof(tag))) {
        return 0;
    }

    char url[256];
    char body[64];
    snprintf(url, sizeof(url), SWAPP_REPO_URL "/releases/download/%s/version.txt", tag);
    if (!swapp_http_get_text(url, NULL, body, sizeof(body))) {
        return 0;
    }
    size_t length = strspn(body, "0123456789abcdef");
    if (length == 0 || length >= hash_size) {
        return 0;
    }
    memcpy(hash, body, length);
    hash[length] = '\0';
    memcpy(g_tag, tag, sizeof(g_tag));
    return 1;
}

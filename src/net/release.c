#include "release.h"
#include "core/util.h"
#include "vfs/vfs_curl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the string value after the first "key": at or after from; GitHub writes plain JSON strings, so an escape only
 * matters for \" and \\, which no tag or URL holds */
static char *json_string(const char *json, const char *key, const char **after)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\t' || *p == '\r') p++;
    if (*p != '"') return NULL;
    const char *end = strchr(++p, '"');
    if (!end) return NULL;
    if (after) *after = end + 1;
    return fr_strndup(p, (size_t)(end - p));
}

int fr_release_parse(const char *json, fr_release *out)
{
    *out = (fr_release){0};
    out->tag = json_string(json, "tag_name", NULL);
    if (!out->tag) return -EINVAL;
    /* the release's own html_url comes before its assets' */
    const char *assets = strstr(json, "\"assets\"");
    char *page = json_string(json, "html_url", NULL);
    if (page && (!assets || strstr(json, page) < assets)) out->page = page;
    else free(page);
    const char *p = assets ? assets : json;
    char *url;
    while ((url = json_string(p, "browser_download_url", &p))) {
        size_t n = strlen(url), an = strlen(FR_RELEASE_LINUX_ASSET);
        if (!out->apk && n > 4 && !strcmp(url + n - 4, ".apk")) out->apk = url;
        else if (!out->linux_bin && n > an && url[n - an - 1] == '/' && !strcmp(url + n - an, FR_RELEASE_LINUX_ASSET)) out->linux_bin = url;
        else free(url);
    }
    return 0;
}

int fr_release_latest(fr_release *out)
{
    fr_curl_buf body = {0};
    int rc = fr_curl_fetch("https://api.github.com/repos/" FR_RELEASE_REPO "/releases/latest", &body, NULL);
    if (!rc) rc = body.n ? fr_release_parse(body.v, out) : -EINVAL;
    free(body.v);
    return rc;
}

void fr_release_free(fr_release *r)
{
    free(r->tag), free(r->page), free(r->apk), free(r->linux_bin);
    *r = (fr_release){0};
}

static void version_parts(const char *v, long out[3])
{
    if (*v == 'v' || *v == 'V') v++;
    for (int i = 0; i < 3; i++) {
        char *end;
        out[i] = strtol(v, &end, 10);
        v = *end == '.' ? end + 1 : end;
        if (*end != '.') {
            for (int k = i + 1; k < 3; k++) out[k] = 0;
            return;
        }
    }
}

int fr_version_cmp(const char *a, const char *b)
{
    long x[3], y[3];
    version_parts(a, x), version_parts(b, y);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

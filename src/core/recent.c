#include "recent.h"
#include <stdlib.h>
#include <string.h>

char *fr_uri_strip_password(const char *uri)
{
    size_t n = strlen(uri);
    char *out = fr_realloc(NULL, n + 1), *o = out;
    const char *p = uri;
    while (*p) {
        const char *sep = strstr(p, "://");
        if (!sep) break;
        const char *auth = sep + 3, *end = auth + strcspn(auth, "/!?#"), *at = NULL, *colon = NULL;
        for (const char *q = auth; q < end; q++)
            if (*q == '@') at = q;
        for (const char *q = auth; at && q < at; q++)
            if (*q == ':') {
                colon = q;
                break;
            }
        if (colon) {
            memcpy(o, p, (size_t)(colon - p));
            o += colon - p;
            p = at;
        } else {
            memcpy(o, p, (size_t)(auth - p));
            o += auth - p;
            p = auth;
        }
    }
    strcpy(o, p);
    return out;
}

void fr_recent_push(fr_strs *list, const char *uri, int max)
{
    char *clean = fr_uri_strip_password(uri);
    for (int i = 0; i < list->n; i++)
        if (!strcmp(list->v[i], clean)) {
            free(list->v[i]);
            memmove(list->v + i, list->v + i + 1, sizeof *list->v * (size_t)(list->n - i - 1));
            list->n--;
            break;
        }
    FR_PUSH(*list, NULL);
    memmove(list->v + 1, list->v, sizeof *list->v * (size_t)(list->n - 1));
    list->v[0] = clean;
    while (list->n > max && list->n > 0) free(list->v[--list->n]);
}

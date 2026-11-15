#include "bookmarks.h"
#include <stdlib.h>
#include <string.h>

int fr_bookmark_key_valid(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static int find(const fr_bookmarks *b, int key)
{
    for (int i = 0; i < b->n; i++)
        if (b->v[i].key == key) return i;
    return -1;
}

const char *fr_bookmark_get(const fr_bookmarks *b, int key)
{
    int i = find(b, key);
    return i < 0 ? NULL : b->v[i].uri;
}

void fr_bookmark_set(fr_bookmarks *b, int key, const char *uri)
{
    if (!fr_bookmark_key_valid(key) || !uri) return;
    int i = find(b, key);
    if (i >= 0) {
        free(b->v[i].uri);
        b->v[i].uri = fr_strdup(uri);
        return;
    }
    int at = 0;
    while (at < b->n && b->v[at].key < key) at++;
    FR_GROW(*b, b->n + 1);
    memmove(b->v + at + 1, b->v + at, sizeof *b->v * (size_t)(b->n - at));
    b->v[at] = (fr_bookmark){(char)key, fr_strdup(uri)};
    b->n++;
}

int fr_bookmark_remove(fr_bookmarks *b, int key)
{
    int i = find(b, key);
    if (i < 0) return 0;
    free(b->v[i].uri);
    memmove(b->v + i, b->v + i + 1, sizeof *b->v * (size_t)(b->n - i - 1));
    b->n--;
    return 1;
}

int fr_bookmark_key_of(const fr_bookmarks *b, const char *uri)
{
    for (int i = 0; i < b->n; i++)
        if (!strcmp(b->v[i].uri, uri)) return b->v[i].key;
    return 0;
}

int fr_bookmark_free_key(const fr_bookmarks *b)
{
    static const char order[] = "123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0";
    for (const char *k = order; *k; k++)
        if (find(b, *k) < 0) return *k;
    return 0;
}

fr_bookmarks fr_bookmarks_copy(const fr_bookmarks *b)
{
    fr_bookmarks r = {0};
    for (int i = 0; i < b->n; i++) FR_PUSH(r, ((fr_bookmark){b->v[i].key, fr_strdup(b->v[i].uri)}));
    return r;
}

int fr_bookmarks_equal(const fr_bookmarks *a, const fr_bookmarks *b)
{
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++)
        if (a->v[i].key != b->v[i].key || strcmp(a->v[i].uri, b->v[i].uri)) return 0;
    return 1;
}

void fr_bookmarks_free(fr_bookmarks *b)
{
    for (int i = 0; i < b->n; i++) free(b->v[i].uri);
    free(b->v);
    *b = (fr_bookmarks){0};
}

void fr_bookmarks_parse(fr_bookmarks *out, const fr_toml *t, char *(*to_uri)(const char *value))
{
    fr_bookmarks_free(out);
    for (int i = 0; i < t->n; i++) {
        const fr_toml_value *v = &t->v[i];
        if (strcmp(v->table, "bookmarks") || v->type != TOML_STRING || strlen(v->key) != 1 || !fr_bookmark_key_valid(v->key[0]))
            continue;
        char *uri = to_uri(v->str);
        if (uri) fr_bookmark_set(out, v->key[0], uri);
        free(uri);
    }
}

void fr_bookmarks_write(fr_toml_out *o, const fr_bookmarks *b, const char *(*shown)(const char *uri))
{
    fr_toml_table(o, "bookmarks");
    for (int i = 0; i < b->n; i++) {
        char key[2] = {b->v[i].key, 0};
        const char *s = shown ? shown(b->v[i].uri) : NULL;
        fr_toml_string(o, key, s ? s : b->v[i].uri);
    }
}

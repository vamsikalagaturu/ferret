#ifndef FR_BOOKMARKS_H
#define FR_BOOKMARKS_H
/* Folders marked with a key each, as ranger's and lf's marks are: m<key> sets one, '<key> goes there. settings.toml
 * keeps them as its [bookmarks] table, one `key = "path or URI"` each. */
#include "core/toml.h"

typedef struct {
    char key;
    char *uri;
} fr_bookmark;

typedef struct {
    fr_bookmark *v;
    int n, cap;
} fr_bookmarks; /* in key order: digits, then capitals, then small letters */

int fr_bookmark_key_valid(int c); /* ASCII letters and digits */
const char *fr_bookmark_get(const fr_bookmarks *b, int key); /* NULL when the key has none */
void fr_bookmark_set(fr_bookmarks *b, int key, const char *uri); /* replacing what the key had */
int fr_bookmark_remove(fr_bookmarks *b, int key);                 /* 1 when the key had one */
int fr_bookmark_key_of(const fr_bookmarks *b, const char *uri);   /* the first key marking uri, 0 none */
int fr_bookmark_free_key(const fr_bookmarks *b);                  /* the first of 1-9, a-z, A-Z, 0 unused; 0 none */
fr_bookmarks fr_bookmarks_copy(const fr_bookmarks *b);
int fr_bookmarks_equal(const fr_bookmarks *a, const fr_bookmarks *b);
void fr_bookmarks_free(fr_bookmarks *b);

/* t's [bookmarks] table into out (emptied first): keys of one letter or digit, each value made a URI by to_uri
 * (malloc'd, NULL when it is not one: skipped) */
void fr_bookmarks_parse(fr_bookmarks *out, const fr_toml *t, char *(*to_uri)(const char *value));
/* the [bookmarks] header and one line per mark, its URI written as shown() gives it (NULL: as it is) */
void fr_bookmarks_write(fr_toml_out *o, const fr_bookmarks *b, const char *(*shown)(const char *uri));
#endif

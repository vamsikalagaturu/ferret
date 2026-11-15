#ifndef FR_UTIL_H
#define FR_UTIL_H
#include <stddef.h>
#include <stdint.h>

/* Growable arrays: any struct with `T *v; int n, cap;`. */
#define FR_GROW(a, need)                                                                                               \
    do {                                                                                                               \
        if ((need) > (a).cap) {                                                                                        \
            int cap_ = (a).cap ? (a).cap * 2 : 16;                                                                     \
            while (cap_ < (need)) cap_ *= 2;                                                                           \
            (a).v = fr_realloc((a).v, sizeof *(a).v * (size_t)cap_);                                                   \
            (a).cap = cap_;                                                                                            \
        }                                                                                                              \
    } while (0)
#define FR_PUSH(a, item)                                                                                               \
    do {                                                                                                               \
        FR_GROW(a, (a).n + 1);                                                                                         \
        (a).v[(a).n++] = (item);                                                                                       \
    } while (0)

typedef struct {
    char **v;
    int n, cap;
} fr_strs;

void *fr_realloc(void *p, size_t size); /* aborts when out of memory */
char *fr_strdup(const char *s);
char *fr_strndup(const char *s, size_t n);
char *fr_format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fr_strs_free(fr_strs *s);
fr_strs fr_strs_copy(const fr_strs *s);

int fr_mkdirs(const char *path); /* 0, or -errno */

/* "report.tar.gz" -> "gz"; "" for none (".bashrc" has none) */
const char *fr_ext(const char *name);
/* length of the name before its extension, the whole length for folders and names without one */
size_t fr_stem_len(const char *name, int is_dir);

/* Natural order, ignoring ASCII case: "file2" < "file10". */
int fr_natcmp(const char *a, const char *b);
/* ASCII-case-insensitive substring */
int fr_contains_nocase(const char *haystack, const char *needle);

void fr_format_size(uint64_t bytes, char *buf, size_t len);
/* relative to now: "14:05" today, "12 Nov" this year, "12 Nov 2024" before */
void fr_format_time(int64_t t, int64_t now, char *buf, size_t len);

/* UTF-8: the byte index after the code point at i (len bounds it), the one before, and the code point itself */
int fr_utf8_next(const char *s, int len, int i);
int fr_utf8_prev(const char *s, int i);
int fr_utf8_decode(const char *s, int len, int i, int *cp);
int fr_utf8_encode(int cp, char out[4]);

double fr_now(void); /* monotonic seconds */

/* to as seen from the folder from_dir, both absolute: "/a/b", "/a/c/d" -> "../c/d"; "." when they are the same */
char *fr_path_relative(const char *from_dir, const char *to);
#endif

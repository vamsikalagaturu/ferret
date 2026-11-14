#define _GNU_SOURCE
#include "util.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

void *fr_realloc(void *p, size_t size)
{
    void *r = realloc(p, size ? size : 1);
    if (!r) abort();
    return r;
}

char *fr_strdup(const char *s) { return fr_strndup(s, strlen(s)); }

char *fr_strndup(const char *s, size_t n)
{
    char *r = fr_realloc(NULL, n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

char *fr_format(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *r = fr_realloc(NULL, (size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(r, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return r;
}

void fr_strs_free(fr_strs *s)
{
    for (int i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    *s = (fr_strs){0};
}

fr_strs fr_strs_copy(const fr_strs *s)
{
    fr_strs r = {0};
    for (int i = 0; i < s->n; i++) FR_PUSH(r, fr_strdup(s->v[i]));
    return r;
}

int fr_mkdirs(const char *path)
{
    char *p = fr_strdup(path);
    int rc = 0;
    for (char *c = p + 1;; c++) {
        if (*c && *c != '/') continue;
        char saved = *c;
        *c = 0;
        if (mkdir(p, 0777) && errno != EEXIST) {
            rc = -errno;
            break;
        }
        *c = saved;
        if (!saved) break;
    }
    free(p);
    return rc;
}

const char *fr_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && dot != name && dot[1] ? dot + 1 : "";
}

size_t fr_stem_len(const char *name, int is_dir)
{
    const char *e = is_dir ? "" : fr_ext(name);
    return *e ? (size_t)(e - 1 - name) : strlen(name);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int digit(int c) { return c >= '0' && c <= '9'; }

int fr_natcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (digit(*a) && digit(*b)) {
            const char *a0 = a, *b0 = b;
            while (*a0 == '0') a0++;
            while (*b0 == '0') b0++;
            const char *a1 = a0, *b1 = b0;
            while (digit(*a1)) a1++;
            while (digit(*b1)) b1++;
            if (a1 - a0 != b1 - b0) return (a1 - a0) < (b1 - b0) ? -1 : 1;
            int c = strncmp(a0, b0, (size_t)(a1 - a0));
            if (c) return c;
            a = a1, b = b1;
            continue;
        }
        int ca = lower((unsigned char)*a), cb = lower((unsigned char)*b);
        if (ca != cb) return ca < cb ? -1 : 1;
        a++, b++;
    }
    if (*a || *b) return *a ? 1 : -1;
    return 0;
}

int fr_contains_nocase(const char *h, const char *n)
{
    if (!*n) return 1;
    for (; *h; h++) {
        const char *x = h, *y = n;
        while (*x && *y && lower((unsigned char)*x) == lower((unsigned char)*y)) x++, y++;
        if (!*y) return 1;
    }
    return 0;
}

void fr_format_size(uint64_t bytes, char *buf, size_t len)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    if (bytes < 1000) {
        snprintf(buf, len, "%llu B", (unsigned long long)bytes);
        return;
    }
    double v = (double)bytes;
    int u = 0;
    while (v >= 1000 && u < 5) v /= 1000, u++;
    snprintf(buf, len, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
}

void fr_format_time(int64_t t, int64_t now, char *buf, size_t len)
{
    time_t tt = (time_t)t, nn = (time_t)now;
    struct tm a, b;
    localtime_r(&tt, &a);
    localtime_r(&nn, &b);
    const char *fmt = a.tm_year == b.tm_year && a.tm_yday == b.tm_yday ? "%H:%M"
                      : a.tm_year == b.tm_year                          ? "%d %b"
                                                                        : "%d %b %Y";
    strftime(buf, len, fmt, &a);
}

int fr_utf8_next(const char *s, int len, int i)
{
    if (i >= len) return len;
    i++;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

int fr_utf8_prev(const char *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

int fr_utf8_decode(const char *s, int len, int i, int *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    int c = u[i], n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (i + n > len) n = 1;
    if (n == 1) {
        *cp = c < 0x80 ? c : 0xFFFD;
        return i + 1;
    }
    int v = c & (0x7F >> n);
    for (int k = 1; k < n; k++) {
        if ((u[i + k] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return i + 1;
        }
        v = (v << 6) | (u[i + k] & 0x3F);
    }
    *cp = v;
    return i + n;
}

int fr_utf8_encode(int cp, char out[4])
{
    if (cp < 0x80) return out[0] = (char)cp, 1;
    if (cp < 0x800) return out[0] = (char)(0xC0 | cp >> 6), out[1] = (char)(0x80 | (cp & 0x3F)), 2;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | cp >> 12), out[1] = (char)(0x80 | (cp >> 6 & 0x3F)), out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | cp >> 18), out[1] = (char)(0x80 | (cp >> 12 & 0x3F));
    out[2] = (char)(0x80 | (cp >> 6 & 0x3F)), out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

double fr_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

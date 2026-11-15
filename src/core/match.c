#include "match.h"
#include "util.h"
#include "fzf.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static const char *next_cp(const char *s)
{
    int n = (int)strlen(s);
    return s + fr_utf8_next(s, n < 4 ? n : 4, 0);
}

/* one star backtracks at a time: the last star seen takes one more character whenever what follows it fails */
int fr_glob_match(const char *p, const char *s)
{
    const char *star = NULL, *resume = NULL;
    while (*s) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            star = p, resume = s;
        } else if (*p == '?') {
            p++, s = next_cp(s);
        } else if (*p && lower((unsigned char)*p) == lower((unsigned char)*s)) {
            p++, s++;
        } else if (star) {
            p = star;
            s = resume = next_cp(resume);
        } else return 0;
    }
    while (*p == '*') p++;
    return !*p;
}

/* fzf's matcher keeps a parsed pattern and a scratch slab (some 200 KB); each thread (a search's, the window's sorting
 * results) keeps its own for the last term, since parsing per name would cost more than matching, and frees it when
 * the thread ends */
typedef struct {
    char *term;
    fzf_pattern_t *pattern;
    fzf_slab_t *slab;
} fuzzy_cache;

static pthread_key_t cache_key;
static pthread_once_t cache_once = PTHREAD_ONCE_INIT;

static void cache_free(void *p)
{
    fuzzy_cache *c = p;
    if (c->pattern) fzf_free_pattern(c->pattern);
    if (c->slab) fzf_free_slab(c->slab);
    free(c->term);
    free(c);
}

static void cache_key_make(void) { pthread_key_create(&cache_key, cache_free); }

int fr_fuzzy_score(const char *term, const char *name)
{
    if (!*term) return 0;
    pthread_once(&cache_once, cache_key_make);
    fuzzy_cache *c = pthread_getspecific(cache_key);
    if (!c) {
        c = fr_realloc(NULL, sizeof *c);
        *c = (fuzzy_cache){0};
        pthread_setspecific(cache_key, c);
    }
    if (!c->term || strcmp(c->term, term)) {
        if (c->pattern) fzf_free_pattern(c->pattern);
        free(c->term);
        c->term = fr_strdup(term);
        c->pattern = fzf_parse_pattern(CaseIgnore, false, c->term, true);
        if (!c->slab) c->slab = fzf_make_default_slab();
    }
    int32_t score = fzf_get_score(name, c->pattern, c->slab);
    return score > 0 ? score : -1;
}

int fr_name_matches(const char *term, const char *name)
{
    if (strpbrk(term, "*?")) return fr_glob_match(term, name);
    return fr_fuzzy_score(term, name) >= 0;
}

static int digits(const char **p, int *out)
{
    const char *s = *p;
    int v = 0, any = 0;
    while (*s >= '0' && *s <= '9' && v < 100000000) v = v * 10 + (*s++ - '0'), any = 1;
    if (!any) return 0;
    *out = v, *p = s;
    return 1;
}

char *fr_number_expand(const char *pattern, int n)
{
    size_t cap = strlen(pattern) + 64, len = 0;
    char *out = fr_realloc(NULL, cap);
    for (const char *p = pattern; *p;) {
        int width = 0, start = 1, ok = 0;
        const char *q = p;
        if (!strncmp(q, "{n", 2)) {
            q += 2;
            if (*q == '}') ok = 1;
            else if (*q == ':' && (q++, digits(&q, &width))) {
                if (*q == '}') ok = 1;
                else if (*q == ':' && (q++, digits(&q, &start)) && *q == '}') ok = 1;
            }
        }
        if (ok && width > 32) width = 32;
        char num[48];
        int k = ok ? snprintf(num, sizeof num, "%0*d", width, start + n - 1) : 0;
        if (len + (size_t)k + 2 > cap) cap = (len + (size_t)k + 2) * 2, out = fr_realloc(out, cap);
        if (ok) {
            memcpy(out + len, num, (size_t)k);
            len += (size_t)k;
            p = q + 1;
        } else out[len++] = *p++;
    }
    out[len] = 0;
    return out;
}

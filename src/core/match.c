#include "match.h"
#include "util.h"
#include "fzf.h"
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

/* fzf's matcher keeps a parsed pattern and a scratch slab; each thread (the search's, the window's sorting results)
 * keeps its own for the last term, since parsing per name would cost more than matching. The last ones of a thread
 * that ends are not freed: a few hundred bytes. */
static _Thread_local struct {
    char *term;
    fzf_pattern_t *pattern;
    fzf_slab_t *slab;
} fz;

int fr_fuzzy_score(const char *term, const char *name)
{
    if (!*term) return 0;
    if (!fz.term || strcmp(fz.term, term)) {
        if (fz.pattern) fzf_free_pattern(fz.pattern);
        free(fz.term);
        fz.term = fr_strdup(term);
        fz.pattern = fzf_parse_pattern(CaseIgnore, false, fz.term, true);
        if (!fz.slab) fz.slab = fzf_make_default_slab();
    }
    int32_t score = fzf_get_score(name, fz.pattern, fz.slab);
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

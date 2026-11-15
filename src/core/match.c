#include "match.h"
#include "util.h"
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

static int word_start(const char *name, int i)
{
    if (!i) return 1;
    char p = name[i - 1], c = name[i];
    return strchr(" _-./()[]", p) || (p >= 'a' && p <= 'z' && c >= 'A' && c <= 'Z') || ((p < '0' || p > '9') && c >= '0' && c <= '9');
}

/* The term's characters in order, others between allowed; ASCII case ignored. Greedy from the left, which can miss a
 * better placement further on but never a match. Matches at word starts and runs of adjacent characters score most,
 * gaps cost a little, a whole-substring match gets a bonus; -1 when the name does not hold the term. */
int fr_fuzzy_score(const char *term, const char *name)
{
    if (!*term) return 0;
    int score = 0, run = 0, last = -1, ti = 0;
    for (int i = 0; name[i] && term[ti]; i++) {
        if (lower((unsigned char)name[i]) != lower((unsigned char)term[ti])) continue;
        int bonus = 1;
        if (word_start(name, i)) bonus += 8;
        if (last == i - 1) run++, bonus += 4 * run;
        else run = 0;
        if (last >= 0) score -= i - last - 1 > 6 ? 6 : i - last - 1;
        score += bonus;
        last = i, ti++;
    }
    if (term[ti]) return -1;
    if (fr_contains_nocase(name, term)) score += 20;
    return score < 0 ? 0 : score;
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

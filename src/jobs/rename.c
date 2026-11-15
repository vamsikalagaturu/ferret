#define _GNU_SOURCE
#include "rename.h"
#include "core/match.h"
#include "vfs/vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char *v;
    size_t n, cap;
} buf;

static void put(buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->v = fr_realloc(b->v, b->cap);
    }
    memcpy(b->v + b->n, s, n);
    b->n += n;
    b->v[b->n] = 0;
}

int fr_rename_rule_compile(fr_rename_rule *r, char *err, size_t errlen)
{
    fr_rename_rule_free(r);
    if (!r->regex || !*r->find) return 0;
    int rc = regcomp(&r->re, r->find, REG_EXTENDED);
    if (rc) {
        char why[200];
        regerror(rc, &r->re, why, sizeof why);
        snprintf(err, errlen, "Regular expression: %s", why);
        return -1;
    }
    r->compiled = 1;
    return 0;
}

void fr_rename_rule_free(fr_rename_rule *r)
{
    if (r->compiled) regfree(&r->re);
    r->compiled = 0;
}

/* rep with \0..\9 taken from the match in s, \\ a backslash */
static void put_groups(buf *b, const char *rep, const char *s, const regmatch_t *m)
{
    for (const char *p = rep; *p; p++) {
        if (*p == '\\' && p[1] >= '0' && p[1] <= '9') {
            int g = p[1] - '0';
            if (m[g].rm_so >= 0) put(b, s + m[g].rm_so, (size_t)(m[g].rm_eo - m[g].rm_so));
            p++;
        } else if (*p == '\\' && p[1] == '\\') put(b, p++, 1);
        else put(b, p, 1);
    }
}

char *fr_rename_rule_apply(const fr_rename_rule *r, const char *name, int is_dir, int n)
{
    char *rep = fr_number_expand(r->replace ? r->replace : "", n);
    buf b = {0};
    put(&b, "", 0);
    if (!*r->find) {
        size_t stem = fr_stem_len(name, is_dir);
        if (*rep) put(&b, rep, strlen(rep)), put(&b, name + stem, strlen(name + stem));
        else put(&b, name, strlen(name));
    } else if (r->regex && r->compiled) {
        const char *s = name;
        regmatch_t m[10];
        int flags = 0;
        while (*s && !regexec(&r->re, s, 10, m, flags)) {
            put(&b, s, (size_t)m[0].rm_so);
            put_groups(&b, rep, s, m);
            if (m[0].rm_eo == m[0].rm_so) {
                if (!s[m[0].rm_eo]) {
                    s += m[0].rm_eo;
                    break;
                }
                put(&b, s + m[0].rm_eo, 1);
                s += m[0].rm_eo + 1;
            } else s += m[0].rm_eo;
            flags = REG_NOTBOL;
        }
        put(&b, s, strlen(s));
    } else if (!r->regex) {
        size_t fl = strlen(r->find);
        const char *s = name, *hit;
        while ((hit = strstr(s, r->find))) {
            put(&b, s, (size_t)(hit - s));
            put(&b, rep, strlen(rep));
            s = hit + fl;
        }
        put(&b, s, strlen(s));
    } else put(&b, name, strlen(name));
    free(rep);
    return b.v;
}

/* ---- planning ---- */

typedef struct {
    char *from, *to;
} pair;

static int moving_away(const pair *p, int n, const char *uri)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(p[i].from, uri)) return 1;
    return 0;
}

/* a name taken by nothing, in from's folder */
static char *temp_name(const pair *p, int n, const char *from)
{
    char *dir = fr_uri_parent(from), *name = fr_uri_name(from);
    for (int k = 1;; k++) {
        char *tmp = fr_format(".ferret-rename-%d-%s", k, name), *uri = fr_uri_join(dir ? dir : from, tmp);
        free(tmp);
        int clash = fr_vfs_exists(uri);
        for (int i = 0; i < n && !clash; i++) clash = !strcmp(p[i].from, uri) || !strcmp(p[i].to, uri);
        if (!clash) {
            free(dir), free(name);
            return uri;
        }
        free(uri);
    }
}

int fr_rename_plan(const fr_strs *from, const fr_strs *to, fr_steps *out, char *err, size_t errlen)
{
    *out = (fr_steps){0};
    int n = from->n < to->n ? from->n : to->n, rc = 0;
    for (int i = 0; i < n && !rc; i++) {
        char *name = fr_uri_name(to->v[i]);
        if (!fr_name_valid(name)) {
            char *old = fr_uri_name(from->v[i]);
            snprintf(err, errlen, "\"%s\" cannot be named \"%s\": a name cannot be empty, \".\", \"..\" or contain \"/\"", old, name);
            free(old);
            rc = -1;
        }
        for (int k = 0; k < i && !rc; k++)
            if (!strcmp(to->v[k], to->v[i])) snprintf(err, errlen, "Two items would be named \"%s\"", name), rc = -1;
        free(name);
    }
    if (rc) return rc;
    pair *p = fr_realloc(NULL, sizeof *p * (size_t)(n ? n : 1));
    int np = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(from->v[i], to->v[i])) p[np++] = (pair){fr_strdup(from->v[i]), fr_strdup(to->v[i])};
    for (int i = 0; i < np && !rc; i++) {
        /* a change of case alone finds the item itself there on a case-insensitive filesystem */
        if (moving_away(p, np, p[i].to) || !strcasecmp(p[i].to, p[i].from) || !fr_vfs_exists(p[i].to)) continue;
        char *name = fr_uri_name(p[i].to);
        snprintf(err, errlen, "\"%s\" already exists", name);
        free(name);
        rc = -1;
    }
    for (int left = rc ? 0 : np; left > 0;) {
        int progress = 0;
        for (int i = 0; i < left; i++) {
            int blocked = 0;
            for (int k = 0; k < left && !blocked; k++) blocked = k != i && !strcmp(p[k].from, p[i].to);
            if (blocked) continue;
            fr_steps_add(out, STEP_RENAME, p[i].from, p[i].to);
            free(p[i].from), free(p[i].to);
            p[i] = p[--left];
            progress = 1;
            i--;
        }
        if (!progress) {
            char *tmp = temp_name(p, left, p[0].from);
            fr_steps_add(out, STEP_RENAME, p[0].from, tmp);
            free(p[0].from);
            p[0].from = tmp;
        }
    }
    for (int i = 0; i < (rc ? np : 0); i++) free(p[i].from), free(p[i].to);
    free(p);
    if (rc) fr_steps_free(out);
    return rc;
}

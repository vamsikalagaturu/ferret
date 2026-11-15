#include "xdg.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *v;
    int n, cap;
} buf;

static void put(buf *b, const char *s, size_t n)
{
    FR_GROW(*b, b->n + (int)n + 1);
    memcpy(b->v + b->n, s, n);
    b->n += (int)n;
    b->v[b->n] = 0;
}

static void put_c(buf *b, char c) { put(b, &c, 1); }

static char *take(buf *b)
{
    if (!b->v) put(b, "", 0);
    char *s = b->v;
    *b = (buf){0};
    return s;
}

/* the line at *p (without its newline or a trailing \r) as [*start, *end); *p moves past it. 0 at the end */
static int line(const char **p, const char **start, const char **end)
{
    if (!**p) return 0;
    const char *s = *p, *e = strchr(s, '\n');
    *p = e ? e + 1 : s + strlen(s);
    if (!e) e = s + strlen(s);
    if (e > s && e[-1] == '\r') e--;
    *start = s, *end = e;
    return 1;
}

static const char *skip_space(const char *s, const char *end)
{
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    return s;
}

static const char *trim_end(const char *s, const char *end)
{
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;
    return end;
}

/* "[name]" on a line: 1 and the name's span */
static int header(const char *s, const char *e, const char **ns, const char **ne)
{
    s = skip_space(s, e), e = trim_end(s, e);
    if (e - s < 2 || *s != '[' || e[-1] != ']') return 0;
    *ns = s + 1, *ne = e - 1;
    return 1;
}

/* "key=value": the trimmed key and the value (leading space dropped) */
static int entry(const char *s, const char *e, const char **ks, const char **ke, const char **vs)
{
    s = skip_space(s, e);
    if (s == e || *s == '#' || *s == '[') return 0;
    const char *eq = memchr(s, '=', (size_t)(e - s));
    if (!eq) return 0;
    *ks = s, *ke = trim_end(s, eq), *vs = skip_space(eq + 1, e);
    return 1;
}

static int span_is(const char *s, const char *e, const char *word) { return (size_t)(e - s) == strlen(word) && !strncmp(s, word, (size_t)(e - s)); }

/* the escapes of a string value: \s \n \t \r \\, and \; inside lists */
static char *unescape(const char *s, const char *e)
{
    buf b = {0};
    for (; s < e; s++) {
        if (*s != '\\' || s + 1 >= e) {
            put_c(&b, *s);
            continue;
        }
        char c = *++s;
        put_c(&b, c == 's' ? ' ' : c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c);
    }
    return take(&b);
}

/* a ';'-separated list into out (empty items skipped, no repeats) */
static void split_list(const char *s, const char *e, fr_strs *out)
{
    while (s < e) {
        const char *p = s;
        while (p < e && *p != ';') p += *p == '\\' && p + 1 < e ? 2 : 1;
        const char *is = skip_space(s, p), *ie = trim_end(is, p);
        if (ie > is) {
            char *item = unescape(is, ie);
            int dup = 0;
            for (int i = 0; i < out->n && !dup; i++) dup = !strcmp(out->v[i], item);
            if (dup) free(item);
            else FR_PUSH(*out, item);
        }
        s = p + 1;
    }
}

static int is_true(const char *s, const char *e) { return span_is(s, e, "true") || span_is(s, e, "1"); }

int fr_desktop_parse(const char *text, fr_desktop_entry *out)
{
    *out = (fr_desktop_entry){0};
    int in = 0, found = 0;
    const char *p = text, *s, *e, *ks, *ke, *vs;
    while (line(&p, &s, &e)) {
        const char *ns, *ne;
        if (header(s, e, &ns, &ne)) {
            in = span_is(ns, ne, "Desktop Entry") && !found;
            found |= in;
            continue;
        }
        if (!in || !entry(s, e, &ks, &ke, &vs)) continue;
        char **field = span_is(ks, ke, "Name") ? &out->name : span_is(ks, ke, "Exec") ? &out->exec
                     : span_is(ks, ke, "TryExec") ? &out->try_exec : span_is(ks, ke, "Icon") ? &out->icon : NULL;
        if (field) {
            free(*field);
            *field = unescape(vs, e);
        } else if (span_is(ks, ke, "Type")) out->application = span_is(vs, trim_end(vs, e), "Application");
        else if (span_is(ks, ke, "Terminal")) out->terminal = is_true(vs, trim_end(vs, e));
        else if (span_is(ks, ke, "Hidden")) out->hidden = is_true(vs, trim_end(vs, e));
        else if (span_is(ks, ke, "NoDisplay")) out->no_display = is_true(vs, trim_end(vs, e));
        else if (span_is(ks, ke, "MimeType")) split_list(vs, e, &out->mime_types);
    }
    return found ? 0 : -1;
}

void fr_desktop_entry_free(fr_desktop_entry *e)
{
    free(e->name), free(e->exec), free(e->try_exec), free(e->icon);
    fr_strs_free(&e->mime_types);
    *e = (fr_desktop_entry){0};
}

/* ---- Exec ---- */

int fr_exec_split(const char *exec, fr_strs *out)
{
    buf b = {0};
    int in_token = 0, rc = 0;
    for (const char *p = exec; *p && !rc;) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n') {
            if (in_token) FR_PUSH(*out, take(&b));
            in_token = 0;
            p++;
            continue;
        }
        in_token = 1;
        if (!b.v) put(&b, "", 0);
        if (c == '"') {
            for (p++; *p && *p != '"'; p++) {
                if (*p == '\\' && p[1] && strchr("\"`$\\", p[1])) p++;
                put_c(&b, *p);
            }
            if (*p) p++;
            else rc = -1;
        } else if (c == '\'') {
            const char *close = strchr(p + 1, '\'');
            if (close) put(&b, p + 1, (size_t)(close - p - 1)), p = close + 1;
            else rc = -1;
        } else if (c == '\\' && p[1]) {
            put_c(&b, p[1]);
            p += 2;
        } else put_c(&b, *p++);
    }
    if (!rc && in_token) FR_PUSH(*out, take(&b));
    free(b.v);
    return rc;
}

void fr_argvs_free(fr_argvs *a)
{
    for (int i = 0; i < a->n; i++) fr_strs_free(&a->v[i]);
    free(a->v);
    *a = (fr_argvs){0};
}

/* a field code of set in tok, %% not counting */
static int has_code(const char *tok, const char *set)
{
    for (const char *p = tok; *p; p++)
        if (*p == '%' && p[1]) {
            if (strchr(set, p[1])) return 1;
            p++;
        }
    return 0;
}

static fr_strs expand_once(const fr_strs *toks, const fr_strs *files, int file, int append, const char *name,
                           const char *icon, const char *location)
{
    fr_strs args = {0}, *argv = &args;
    for (int t = 0; t < toks->n; t++) {
        const char *tok = toks->v[t];
        if (!strcmp(tok, "%F") || !strcmp(tok, "%U")) {
            for (int i = 0; i < files->n; i++) FR_PUSH(*argv, fr_strdup(files->v[i]));
            continue;
        }
        if (!strcmp(tok, "%i")) {
            if (icon && *icon) {
                FR_PUSH(*argv, fr_strdup("--icon"));
                FR_PUSH(*argv, fr_strdup(icon));
            }
            continue;
        }
        buf b = {0};
        int coded = 0;
        for (const char *p = tok; *p; p++) {
            if (*p != '%' || !p[1]) {
                put_c(&b, *p);
                continue;
            }
            char c = *++p;
            if (c == '%') {
                put_c(&b, '%');
                continue;
            }
            coded = 1;
            const char *v = c == 'f' || c == 'u' ? (file >= 0 ? files->v[file] : NULL) : c == 'c' ? name : c == 'k' ? location : NULL;
            if (v) put(&b, v, strlen(v));
        }
        if (coded && !b.n) free(b.v);
        else FR_PUSH(*argv, take(&b));
    }
    if (append && file >= 0) FR_PUSH(*argv, fr_strdup(files->v[file]));
    return args;
}

int fr_exec_expand(const char *exec, const fr_strs *files, const char *name, const char *icon, const char *location,
                   fr_argvs *out)
{
    fr_strs toks = {0};
    if (fr_exec_split(exec, &toks) || !toks.n) {
        fr_strs_free(&toks);
        return -1;
    }
    int list = 0, single = 0;
    for (int t = 0; t < toks.n; t++) {
        list |= !strcmp(toks.v[t], "%F") || !strcmp(toks.v[t], "%U");
        single |= has_code(toks.v[t], "fu");
    }
    int rc = 0;
    if (list || !files->n) FR_PUSH(*out, expand_once(&toks, files, -1, 0, name, icon, location));
    else
        for (int i = 0; i < files->n; i++) FR_PUSH(*out, expand_once(&toks, files, i, !single, name, icon, location));
    for (int i = 0; i < out->n; i++)
        if (!out->v[i].n) rc = -1;
    fr_strs_free(&toks);
    if (rc) fr_argvs_free(out);
    return rc;
}

/* ---- mimeapps.list ---- */

void fr_mimeapps_get(const char *text, const char *group, const char *mime, fr_strs *out)
{
    int in = 0;
    const char *p = text, *s, *e, *ks, *ke, *vs, *ns, *ne;
    while (line(&p, &s, &e)) {
        if (header(s, e, &ns, &ne)) in = span_is(ns, ne, group);
        else if (in && entry(s, e, &ks, &ke, &vs) && span_is(ks, ke, mime)) split_list(vs, e, out);
    }
}

char *fr_mimeapps_set_default(const char *text, const char *mime, const char *id)
{
    if (!text) text = "";
    buf b = {0};
    char *entry_line = fr_format("%s=%s\n", mime, id);
    const char *p = text, *s, *e, *ks, *ke, *vs, *ns, *ne;
    const char *after_last = NULL; /* where the group's last non-blank line ends */
    int in = 0, done = 0;
    while (!done && line(&p, &s, &e)) {
        if (header(s, e, &ns, &ne)) {
            if (in) break;
            in = span_is(ns, ne, "Default Applications");
            if (in) after_last = p;
            continue;
        }
        if (!in) continue;
        if (entry(s, e, &ks, &ke, &vs) && span_is(ks, ke, mime)) {
            put(&b, text, (size_t)(s - text));
            put(&b, entry_line, strlen(entry_line));
            put(&b, p, strlen(p));
            done = 1;
        } else if (skip_space(s, e) < e) after_last = p;
    }
    if (!done && after_last) {
        size_t head = (size_t)(after_last - text);
        put(&b, text, head);
        if (head && text[head - 1] != '\n') put_c(&b, '\n');
        put(&b, entry_line, strlen(entry_line));
        put(&b, after_last, strlen(after_last));
    } else if (!done) {
        size_t n = strlen(text);
        put(&b, text, n);
        if (n && text[n - 1] != '\n') put_c(&b, '\n');
        if (n) put_c(&b, '\n');
        put(&b, "[Default Applications]\n", 23);
        put(&b, entry_line, strlen(entry_line));
    }
    free(entry_line);
    return take(&b);
}

/* ---- shared-mime-info ---- */

/* the two words of a "first second" line */
static int pair(const char *s, const char *e, const char **as, const char **ae, const char **bs, const char **be)
{
    s = skip_space(s, e);
    if (s == e || *s == '#') return 0;
    const char *sp = s;
    while (sp < e && *sp != ' ' && *sp != '\t') sp++;
    if (sp == e) return 0;
    *as = s, *ae = sp;
    *bs = skip_space(sp, e), *be = trim_end(*bs, e);
    return *be > *bs;
}

char *fr_mime_unalias(const char *aliases, const char *mime)
{
    const char *p = aliases ? aliases : "", *s, *e, *as, *ae, *bs, *be;
    while (line(&p, &s, &e))
        if (pair(s, e, &as, &ae, &bs, &be) && span_is(as, ae, mime)) return fr_strndup(bs, (size_t)(be - bs));
    return fr_strdup(mime);
}

void fr_mime_ancestors(const char *subclasses, const char *mime, fr_strs *out)
{
    FR_PUSH(*out, fr_strdup(mime));
    for (int i = 0; i < out->n; i++) {
        const char *p = subclasses ? subclasses : "", *s, *e, *as, *ae, *bs, *be;
        while (line(&p, &s, &e)) {
            if (!pair(s, e, &as, &ae, &bs, &be) || !span_is(as, ae, out->v[i])) continue;
            int seen = 0;
            for (int k = 0; k < out->n && !seen; k++) seen = span_is(bs, be, out->v[k]);
            if (!seen) FR_PUSH(*out, fr_strndup(bs, (size_t)(be - bs)));
        }
    }
    int seen = 0;
    for (int k = 0; k < out->n && !seen; k++) seen = !strcmp(out->v[k], "text/plain");
    if (!seen && !strncmp(mime, "text/", 5)) FR_PUSH(*out, fr_strdup("text/plain"));
}

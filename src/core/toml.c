#include "toml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    int line;
} cursor;

static void put(fr_toml_out *o, const char *s, size_t n)
{
    FR_GROW(*o, o->n + (int)n + 1);
    memcpy(o->v + o->n, s, n);
    o->n += (int)n;
    o->v[o->n] = 0;
}

static void puts_(fr_toml_out *o, const char *s) { put(o, s, strlen(s)); }

static void skip_blank(cursor *c)
{
    while (*c->s == ' ' || *c->s == '\t') c->s++;
}

/* blanks, comments and line breaks */
static void skip_space(cursor *c)
{
    for (;;) {
        skip_blank(c);
        if (*c->s == '#')
            while (*c->s && *c->s != '\n') c->s++;
        if (*c->s == '\r') c->s++;
        else if (*c->s == '\n') c->s++, c->line++;
        else return;
    }
}

static int line_end(cursor *c)
{
    skip_blank(c);
    if (*c->s == '#')
        while (*c->s && *c->s != '\n') c->s++;
    if (*c->s == '\r') c->s++;
    return *c->s == '\n' || !*c->s;
}

static int hex_digits(const char *s, int n, long *out)
{
    long v = 0;
    for (int i = 0; i < n; i++) {
        char h = s[i];
        int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v * 16 + d;
    }
    *out = v;
    return 1;
}

static char *parse_string(cursor *c)
{
    char quote = *c->s;
    if (quote != '"' && quote != '\'') return NULL;
    if (c->s[1] == quote && c->s[2] == quote) return NULL; /* multi-line strings are not in the subset */
    c->s++;
    fr_toml_out o = {0};
    put(&o, "", 0);
    for (;;) {
        char ch = *c->s;
        if (!ch || ch == '\n') {
            free(o.v);
            return NULL;
        }
        c->s++;
        if (ch == quote) return o.v;
        if (ch != '\\' || quote == '\'') {
            put(&o, &ch, 1);
            continue;
        }
        char e = *c->s++, out = 0;
        switch (e) {
        case 'b': out = '\b'; break;
        case 't': out = '\t'; break;
        case 'n': out = '\n'; break;
        case 'f': out = '\f'; break;
        case 'r': out = '\r'; break;
        case '"': out = '"'; break;
        case '\\': out = '\\'; break;
        case 'u':
        case 'U': {
            int n = e == 'u' ? 4 : 8;
            long cp;
            char utf8[4];
            if (!hex_digits(c->s, n, &cp) || cp > 0x10FFFF) {
                free(o.v);
                return NULL;
            }
            c->s += n;
            put(&o, utf8, (size_t)fr_utf8_encode((int)cp, utf8));
            continue;
        }
        default: free(o.v); return NULL;
        }
        put(&o, &out, 1);
    }
}

static int bare_char(char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'; }

static char *parse_key(cursor *c)
{
    if (*c->s == '"' || *c->s == '\'') return parse_string(c);
    const char *start = c->s;
    while (bare_char(*c->s)) c->s++;
    return c->s > start ? fr_strndup(start, (size_t)(c->s - start)) : NULL;
}

static int parse_value(cursor *c, fr_toml_value *v)
{
    if (*c->s == '"' || *c->s == '\'') {
        v->type = TOML_STRING;
        return (v->str = parse_string(c)) != NULL;
    }
    for (int b = 0; b < 2; b++) {
        const char *word = b ? "true" : "false";
        size_t n = strlen(word);
        if (!strncmp(c->s, word, n) && !bare_char(c->s[n])) {
            c->s += n;
            v->type = TOML_BOOL, v->b = b;
            return 1;
        }
    }
    if (*c->s != '[') return 0;
    c->s++;
    v->type = TOML_STRINGS;
    for (;;) {
        skip_space(c);
        if (*c->s == ']') break;
        char *s = parse_string(c);
        if (!s) return 0;
        FR_PUSH(v->strs, s);
        skip_space(c);
        if (*c->s == ',') c->s++;
        else if (*c->s != ']') return 0;
    }
    c->s++;
    return 1;
}

static void value_free(fr_toml_value *v)
{
    free(v->table), free(v->key), free(v->str);
    fr_strs_free(&v->strs);
}

fr_toml fr_toml_parse(const char *text)
{
    fr_toml t = {0};
    cursor c = {text, 1};
    char *table = fr_strdup("");
    for (;;) {
        skip_space(&c);
        if (!*c.s) break;
        int ok = 0;
        if (*c.s == '[') {
            c.s++;
            skip_blank(&c);
            char *name = parse_key(&c);
            skip_blank(&c);
            if (name && *c.s == ']') {
                c.s++;
                if ((ok = line_end(&c))) free(table), table = name, name = NULL;
            }
            free(name);
        } else {
            fr_toml_value v = {0};
            v.key = parse_key(&c);
            skip_blank(&c);
            if (v.key && *c.s == '=') {
                c.s++;
                skip_blank(&c);
                ok = parse_value(&c, &v) && line_end(&c);
            }
            if (ok) {
                v.table = fr_strdup(table);
                FR_PUSH(t, v);
            } else value_free(&v);
        }
        if (!ok) {
            if (!t.error_line) t.error_line = c.line;
            while (*c.s && *c.s != '\n') c.s++;
        }
    }
    free(table);
    return t;
}

const fr_toml_value *fr_toml_get(const fr_toml *t, const char *table, const char *key, int type)
{
    for (int i = t->n - 1; i >= 0; i--)
        if (!strcmp(t->v[i].table, table) && !strcmp(t->v[i].key, key)) return t->v[i].type == type ? &t->v[i] : NULL;
    return NULL;
}

void fr_toml_free(fr_toml *t)
{
    for (int i = 0; i < t->n; i++) value_free(&t->v[i]);
    free(t->v);
    *t = (fr_toml){0};
}

static void put_quoted(fr_toml_out *o, const char *s)
{
    puts_(o, "\"");
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        char esc[8];
        if (ch == '"') puts_(o, "\\\"");
        else if (ch == '\\') puts_(o, "\\\\");
        else if (ch == '\n') puts_(o, "\\n");
        else if (ch == '\t') puts_(o, "\\t");
        else if (ch == '\r') puts_(o, "\\r");
        else if (ch < 0x20 || ch == 0x7f) snprintf(esc, sizeof esc, "\\u%04X", ch), puts_(o, esc);
        else put(o, s, 1);
    }
    puts_(o, "\"");
}

static void put_key(fr_toml_out *o, const char *key)
{
    int bare = *key != 0;
    for (const char *k = key; *k; k++) bare &= bare_char(*k);
    if (bare) puts_(o, key);
    else put_quoted(o, key);
    puts_(o, " = ");
}

void fr_toml_comment(fr_toml_out *o, const char *text)
{
    if (*text) puts_(o, "# "), puts_(o, text);
    puts_(o, "\n");
}

void fr_toml_table(fr_toml_out *o, const char *name)
{
    puts_(o, "[");
    puts_(o, name);
    puts_(o, "]\n");
}

void fr_toml_string(fr_toml_out *o, const char *key, const char *value)
{
    put_key(o, key);
    put_quoted(o, value);
    puts_(o, "\n");
}

void fr_toml_bool(fr_toml_out *o, const char *key, int value)
{
    put_key(o, key);
    puts_(o, value ? "true\n" : "false\n");
}

void fr_toml_strings(fr_toml_out *o, const char *key, const fr_strs *values)
{
    put_key(o, key);
    if (!values->n) {
        puts_(o, "[]\n");
        return;
    }
    puts_(o, "[\n");
    for (int i = 0; i < values->n; i++) {
        puts_(o, "    ");
        put_quoted(o, values->v[i]);
        puts_(o, ",\n");
    }
    puts_(o, "]\n");
}

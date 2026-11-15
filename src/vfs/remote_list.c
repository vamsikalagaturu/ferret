#define _GNU_SOURCE
#include "remote_list.h"
#include "net/net.h"
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <expat.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---- FTP ---- */

static int digits(const char *s, size_t n)
{
    if (!n) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

static long number(const char *s, size_t n)
{
    long v = 0;
    for (size_t i = 0; i < n && v < 100000000L; i++) v = v * 10 + (s[i] - '0');
    return v;
}

/* YYYYMMDDHHMMSS[.sss], UTC as RFC 3659 has it; 0 when it is not that */
static int64_t mlsd_time(const char *s, size_t n)
{
    if (n < 14 || !digits(s, 14)) return 0;
    struct tm tm = {0};
    tm.tm_year = (int)number(s, 4) - 1900;
    tm.tm_mon = (int)number(s + 4, 2) - 1;
    tm.tm_mday = (int)number(s + 6, 2);
    tm.tm_hour = (int)number(s + 8, 2);
    tm.tm_min = (int)number(s + 10, 2);
    tm.tm_sec = (int)number(s + 12, 2);
    return (int64_t)timegm(&tm);
}

static void named(fr_entry *e, const char *name, size_t n)
{
    e->name = fr_strndup(name, n);
    e->hidden = name[0] == '.';
}

int fr_ftp_mlsd_line(const char *line, size_t n, fr_entry *e)
{
    const char *sp = memchr(line, ' ', n);
    if (!sp || sp + 1 >= line + n) return 0;
    const char *name = sp + 1;
    size_t nn = (size_t)(line + n - name);
    if ((nn == 1 && name[0] == '.') || (nn == 2 && !memcmp(name, "..", 2)) || memchr(name, '/', nn)) return 0;
    fr_entry got = {0};
    for (const char *f = line; f < sp;) {
        const char *end = memchr(f, ';', (size_t)(sp - f));
        if (!end) end = sp;
        const char *eq = memchr(f, '=', (size_t)(end - f));
        if (eq) {
            size_t kn = (size_t)(eq - f), vn = (size_t)(end - eq - 1);
            const char *v = eq + 1;
            if (kn == 4 && !strncasecmp(f, "type", 4)) {
                if ((vn == 4 && !strncasecmp(v, "cdir", 4)) || (vn == 4 && !strncasecmp(v, "pdir", 4))) return 0;
                got.is_dir = vn == 3 && !strncasecmp(v, "dir", 3);
                /* "OS.unix=symlink" (ProFTPD), "OS.unix=slink:/target" (others) */
                got.is_link = vn >= 13 && !strncasecmp(v, "OS.unix=", 8) &&
                              (!strncasecmp(v + 8, "slink", 5) || !strncasecmp(v + 8, "symli", 5));
            } else if (kn == 4 && !strncasecmp(f, "size", 4) && digits(v, vn)) {
                got.size = strtoull(v, NULL, 10);
            } else if (kn == 6 && !strncasecmp(f, "modify", 6)) {
                got.mtime = mlsd_time(v, vn);
            } else if (kn == 9 && !strncasecmp(f, "unix.mode", 9) && vn) {
                got.mode = (uint32_t)strtoul(v, NULL, 8) & 07777;
            }
        }
        f = end + 1;
    }
    if (got.is_dir) got.size = 0;
    *e = got;
    named(e, name, nn);
    return 1;
}

static uint32_t mode_of(const char *p)
{
    static const uint32_t bit[9] = {0400, 0200, 0100, 040, 020, 010, 04, 02, 01};
    uint32_t m = 0;
    for (int i = 0; i < 9; i++) {
        char c = p[i + 1];
        if (c != '-' && c != 'S' && c != 'T') m |= bit[i];
    }
    if (p[3] == 's' || p[3] == 'S') m |= 04000;
    if (p[6] == 's' || p[6] == 'S') m |= 02000;
    if (p[9] == 't' || p[9] == 'T') m |= 01000;
    if (p[3] == 'S') m &= ~0100u;
    if (p[6] == 'S') m &= ~010u;
    if (p[9] == 'T') m &= ~01u;
    return m;
}

static int month_of(const char *s, size_t n)
{
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    if (n != 3) return -1;
    for (int i = 0; i < 12; i++)
        if (!strncasecmp(s, months + i * 3, 3)) return i;
    return -1;
}

typedef struct {
    const char *s;
    size_t n;
} word;

/* the Windows (IIS) format: "11-15-26  03:12AM       <DIR>          name" */
static int dos_line(const char *line, size_t n, const word *w, int nw, fr_entry *e)
{
    if (nw < 4 || w[0].n != 8 || w[0].s[2] != '-' || w[0].s[5] != '-' || w[1].n != 7 || w[1].s[2] != ':') return 0;
    int mon = (int)number(w[0].s, 2), day = (int)number(w[0].s + 3, 2), year = (int)number(w[0].s + 6, 2);
    int hour = (int)number(w[1].s, 2) % 12, min = (int)number(w[1].s + 3, 2);
    if (w[1].s[5] == 'P' || w[1].s[5] == 'p') hour += 12;
    fr_entry got = {0};
    if (w[2].n == 5 && !memcmp(w[2].s, "<DIR>", 5)) got.is_dir = 1;
    else if (digits(w[2].s, w[2].n)) got.size = strtoull(w[2].s, NULL, 10);
    else return 0;
    const char *name = w[2].s + w[2].n;
    while (name < line + n && *name == ' ') name++;
    size_t nn = (size_t)(line + n - name);
    if (!nn || (nn == 1 && *name == '.') || (nn == 2 && !memcmp(name, "..", 2))) return 0;
    struct tm tm = {.tm_year = (year < 70 ? year + 100 : year), .tm_mon = mon - 1, .tm_mday = day, .tm_hour = hour, .tm_min = min};
    got.mtime = (int64_t)timegm(&tm);
    *e = got;
    named(e, name, nn);
    return 1;
}

int fr_ftp_list_line(const char *line, size_t n, int64_t now, fr_entry *e)
{
    word w[16];
    int nw = 0;
    for (size_t i = 0; i < n && nw < 16;) {
        while (i < n && line[i] == ' ') i++;
        if (i >= n) break;
        size_t start = i;
        while (i < n && line[i] != ' ') i++;
        w[nw++] = (word){line + start, i - start};
    }
    if (nw && isdigit((unsigned char)line[0])) return dos_line(line, n, w, nw, e);
    if (nw < 5 || w[0].n < 10 || !strchr("-dlbcps", w[0].s[0])) return 0;
    for (int i = 2; i + 2 < nw; i++) {
        int mon = month_of(w[i].s, w[i].n);
        if (mon < 0 || !digits(w[i + 1].s, w[i + 1].n) || !digits(w[i - 1].s, w[i - 1].n)) continue;
        const word *t = &w[i + 2];
        struct tm tm = {.tm_mon = mon, .tm_mday = (int)number(w[i + 1].s, w[i + 1].n)};
        int has_year = digits(t->s, t->n) && t->n == 4;
        if (has_year) tm.tm_year = (int)number(t->s, 4) - 1900;
        else if (t->n >= 4 && t->n <= 5 && t->s[t->n - 3] == ':') {
            tm.tm_hour = (int)number(t->s, t->n - 3);
            tm.tm_min = (int)number(t->s + t->n - 2, 2);
            time_t nt = (time_t)now;
            struct tm cur;
            gmtime_r(&nt, &cur);
            tm.tm_year = cur.tm_year;
            /* ls leaves the year out for the last six months: a date ahead of now is last year's */
            if ((int64_t)timegm(&(struct tm){.tm_year = tm.tm_year, .tm_mon = tm.tm_mon, .tm_mday = tm.tm_mday,
                                             .tm_hour = tm.tm_hour, .tm_min = tm.tm_min}) > now + 2 * 86400)
                tm.tm_year--;
        } else continue;
        const char *name = t->s + t->n + 1;
        if (name >= line + n) return 0;
        size_t nn = (size_t)(line + n - name);
        fr_entry got = {0};
        got.is_dir = w[0].s[0] == 'd';
        got.is_link = w[0].s[0] == 'l';
        if (got.is_link) {
            const char *arrow = memmem(name, nn, " -> ", 4);
            if (arrow) nn = (size_t)(arrow - name);
        }
        if (!nn || (nn == 1 && *name == '.') || (nn == 2 && !memcmp(name, "..", 2))) return 0;
        got.size = got.is_dir ? 0 : strtoull(w[i - 1].s, NULL, 10);
        got.mode = mode_of(w[0].s);
        got.mtime = (int64_t)timegm(&tm);
        *e = got;
        named(e, name, nn);
        return 1;
    }
    return 0;
}

void fr_ftp_listing(const char *buf, size_t n, int mlsd, int64_t now, fr_entries *out)
{
    for (size_t i = 0; i < n;) {
        const char *nl = memchr(buf + i, '\n', n - i);
        size_t end = nl ? (size_t)(nl - buf) : n, len = end - i;
        if (len && buf[i + len - 1] == '\r') len--;
        fr_entry e;
        if (len && (mlsd ? fr_ftp_mlsd_line(buf + i, len, &e) : fr_ftp_list_line(buf + i, len, now, &e))) FR_PUSH(*out, e);
        i = end + 1;
    }
}

int fr_ftp_mlst_reply(const char *reply, fr_entry *e)
{
    for (const char *p = reply; *p;) {
        const char *nl = strchr(p, '\n'), *end = nl ? nl : p + strlen(p);
        size_t len = (size_t)(end - p);
        if (len && p[len - 1] == '\r') len--;
        if (len > 1 && p[0] == ' ') {
            const char *facts = p + 1;
            size_t fn = len - 1;
            const char *sp = memchr(facts, ' ', fn);
            if (sp) {
                /* the pathname is the whole path; only the facts matter here */
                fr_entry got;
                char *line = fr_format("%.*s x", (int)(sp - facts), facts);
                int ok = fr_ftp_mlsd_line(line, strlen(line), &got);
                free(line);
                if (ok) {
                    free(got.name);
                    got.name = NULL;
                    got.hidden = 0;
                    *e = got;
                    return 0;
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    return -ENOENT;
}

/* ---- HTTP and WebDAV ---- */

int fr_http_errno(long status)
{
    if (status >= 200 && status < 300) return 0;
    switch (status) {
    case 401:
    case 407: return -FR_EAUTH;
    case 403: return -EACCES;
    case 404:
    case 410: return -ENOENT;
    case 405: return -EPERM;
    case 409: return -ENOENT; /* RFC 4918: a parent folder is missing */
    case 412: return -EEXIST; /* Overwrite: F, If-None-Match: * */
    case 413: return -EFBIG;
    case 414: return -ENAMETOOLONG;
    case 415:
    case 501: return -ENOTSUP;
    case 423: return -EBUSY;
    case 408:
    case 504: return -ETIMEDOUT;
    case 502:
    case 503: return -EAGAIN;
    case 507: return -ENOSPC;
    default: return -EIO;
    }
}

char *fr_url_path(const char *path)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(path);
    char *out = fr_realloc(NULL, n * 3 + 1), *o = out;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (isalnum(*p) || strchr("/-._~", *p)) *o++ = (char)*p;
        else *o++ = '%', *o++ = hex[*p >> 4], *o++ = hex[*p & 15];
    }
    *o = 0;
    return out;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *fr_url_unescape(const char *s, size_t n)
{
    char *out = fr_realloc(NULL, n + 1), *o = out;
    for (size_t i = 0; i < n; i++) {
        int a, b;
        if (s[i] == '%' && i + 2 < n && (a = hexval(s[i + 1])) >= 0 && (b = hexval(s[i + 2])) >= 0) {
            *o++ = (char)(a << 4 | b);
            i += 2;
        } else *o++ = s[i];
    }
    *o = 0;
    return out;
}

typedef struct {
    int in_response, in_propstat, in_prop, in_resourcetype;
    int text_for; /* which element's text collects: one of the T_ below */
    char *text;
    size_t text_n;
    /* the response being read */
    char *href;
    fr_entry e, pending;
    int pending_status, pending_has;
    int64_t avail, used, p_avail, p_used;
    int ok; /* any 2xx propstat in this response */
    /* the answer */
    const char *self;
    fr_entries *children;
    fr_entry *self_out;
    int got_self, multistatus;
    int64_t *q_avail, *q_used;
} dav_parse;

enum { T_NONE, T_HREF, T_STATUS, T_LENGTH, T_MODIFIED, T_AVAIL, T_USED };

static int is(const char *name, const char *local) { return !strncmp(name, "DAV: ", 5) && !strcmp(name + 5, local); }

static void dav_start(void *ud, const XML_Char *name, const XML_Char **attrs)
{
    dav_parse *d = ud;
    d->text_for = T_NONE;
    d->text_n = 0;
    if (is(name, "multistatus")) d->multistatus = 1;
    else if (is(name, "response")) {
        d->in_response = 1;
        free(d->href);
        d->href = NULL;
        d->e = (fr_entry){0};
        d->ok = 0;
        d->avail = d->used = -1;
    } else if (!d->in_response) return;
    else if (is(name, "href") && !d->in_propstat) d->text_for = T_HREF;
    else if (is(name, "propstat")) {
        d->in_propstat = 1;
        d->pending = (fr_entry){0};
        d->pending_status = 200;
        d->p_avail = d->p_used = -1;
    } else if (d->in_propstat && is(name, "status")) d->text_for = T_STATUS;
    else if (d->in_propstat && is(name, "prop")) d->in_prop = 1;
    else if (d->in_prop && is(name, "resourcetype")) d->in_resourcetype = 1;
    else if (d->in_resourcetype && is(name, "collection")) d->pending.is_dir = 1;
    else if (d->in_prop && is(name, "getcontentlength")) d->text_for = T_LENGTH;
    else if (d->in_prop && is(name, "getlastmodified")) d->text_for = T_MODIFIED;
    else if (d->in_prop && is(name, "quota-available-bytes")) d->text_for = T_AVAIL;
    else if (d->in_prop && is(name, "quota-used-bytes")) d->text_for = T_USED;
}

static void dav_text(void *ud, const XML_Char *s, int len)
{
    dav_parse *d = ud;
    if (d->text_for == T_NONE || len <= 0 || d->text_n > 65536) return;
    d->text = fr_realloc(d->text, d->text_n + (size_t)len + 1);
    memcpy(d->text + d->text_n, s, (size_t)len);
    d->text_n += (size_t)len;
    d->text[d->text_n] = 0;
}

static char *trimmed(dav_parse *d)
{
    if (!d->text_n) return fr_strdup("");
    char *s = d->text, *end = s + d->text_n;
    while (s < end && isspace((unsigned char)*s)) s++;
    while (end > s && isspace((unsigned char)end[-1])) end--;
    return fr_strndup(s, (size_t)(end - s));
}

/* the path of an href, which may be an absolute URL: unescaped, without trailing "/" (but "/" itself) */
static char *href_path(const char *href)
{
    const char *p = href, *sep = strstr(href, "://");
    if (sep) {
        p = strchr(sep + 3, '/');
        if (!p) p = "/";
    }
    const char *q = strpbrk(p, "?#");
    char *path = fr_url_unescape(p, q ? (size_t)(q - p) : strlen(p));
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    return path;
}

static int same_path(const char *a, const char *b)
{
    size_t an = strlen(a), bn = strlen(b);
    while (an > 1 && a[an - 1] == '/') an--;
    while (bn > 1 && b[bn - 1] == '/') bn--;
    return an == bn && !memcmp(a, b, an);
}

static void response_done(dav_parse *d)
{
    if (!d->href || !d->ok) return;
    char *path = href_path(d->href);
    if (same_path(path, d->self)) {
        if (!d->got_self) {
            d->got_self = 1;
            if (d->q_avail) *d->q_avail = d->avail;
            if (d->q_used) *d->q_used = d->used;
            if (d->self_out) {
                const char *slash = strrchr(path, '/'), *nm = slash && slash[1] ? slash + 1 : "/";
                *d->self_out = d->e;
                named(d->self_out, nm, strlen(nm));
            }
        }
    } else if (d->children) {
        const char *slash = strrchr(path, '/');
        const char *name = slash ? slash + 1 : path;
        if (*name && strcmp(name, ".") && strcmp(name, "..")) {
            fr_entry e = d->e;
            named(&e, name, strlen(name));
            FR_PUSH(*d->children, e);
        }
    }
    free(path);
}

static void dav_end(void *ud, const XML_Char *name)
{
    dav_parse *d = ud;
    int what = d->text_for;
    d->text_for = T_NONE;
    if (what != T_NONE) {
        char *t = trimmed(d);
        switch (what) {
        case T_HREF:
            free(d->href);
            d->href = t, t = NULL;
            break;
        case T_STATUS: {
            const char *sp = strchr(t, ' ');
            d->pending_status = sp ? atoi(sp + 1) : 0;
            break;
        }
        case T_LENGTH: d->pending.size = strtoull(t, NULL, 10); break;
        case T_MODIFIED: {
            time_t when = curl_getdate(t, NULL);
            if (when > 0) d->pending.mtime = (int64_t)when;
            break;
        }
        case T_AVAIL: d->p_avail = *t ? strtoll(t, NULL, 10) : -1; break;
        case T_USED: d->p_used = *t ? strtoll(t, NULL, 10) : -1; break;
        }
        free(t);
        d->text_n = 0;
        return;
    }
    if (is(name, "resourcetype")) d->in_resourcetype = 0;
    else if (is(name, "prop")) d->in_prop = 0;
    else if (is(name, "propstat")) {
        d->in_propstat = 0;
        if (d->pending_status >= 200 && d->pending_status < 300) {
            d->ok = 1;
            if (d->pending.is_dir) d->e.is_dir = 1;
            if (d->pending.size) d->e.size = d->pending.size;
            if (d->pending.mtime) d->e.mtime = d->pending.mtime;
            if (d->p_avail >= 0) d->avail = d->p_avail;
            if (d->p_used >= 0) d->used = d->p_used;
        }
    } else if (is(name, "response")) {
        d->in_response = 0;
        if (d->e.is_dir) d->e.size = 0;
        response_done(d);
    }
}

int fr_dav_multistatus(const char *xml, size_t n, const char *self, fr_entries *children, fr_entry *self_out,
                       int64_t *avail, int64_t *used)
{
    XML_Parser p = XML_ParserCreateNS(NULL, ' ');
    if (!p) return -ENOMEM;
    if (avail) *avail = -1;
    if (used) *used = -1;
    dav_parse d = {.self = self, .children = children, .self_out = self_out, .q_avail = avail, .q_used = used};
    XML_SetUserData(p, &d);
    XML_SetElementHandler(p, dav_start, dav_end);
    XML_SetCharacterDataHandler(p, dav_text);
    int ok = XML_Parse(p, xml, (int)n, 1) == XML_STATUS_OK;
    XML_ParserFree(p);
    free(d.text);
    free(d.href);
    if (!ok || !d.multistatus) {
        if (children) fr_entries_free(children);
        if (self_out && d.got_self) fr_entry_free(self_out);
        return -EPROTO;
    }
    return self_out && !d.got_self ? -ENOENT : 0;
}

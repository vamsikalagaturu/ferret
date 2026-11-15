/* ftp://[user@]host[:port]/path and ftps:// (explicit TLS, RFC 4217; implicit on port 990) over libcurl. Paths are
 * the server's absolute ones. Listings use MLSD where the server has it (RFC 3659), else LIST; the server's FEAT
 * answer is asked once per run. An address without a user signs in anonymously. */
#define _GNU_SOURCE
#include "remote_list.h"
#include "vfs_curl.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

enum { F_KNOWN = 1, F_MLST = 2, F_MFMT = 4, F_NO_MLSD = 8 };

static int quotable(const char *path) { return !strpbrk(path, "\r\n"); }

static unsigned features_get(const fr_curl *h) { return __atomic_load_n(&h->srv->features, __ATOMIC_RELAXED); }

static void features_add(fr_curl *h, unsigned f) { __atomic_fetch_or(&h->srv->features, f, __ATOMIC_RELAXED); }

/* commands on the control connection alone (no transfer), each reply collected in h->reply */
static int commands(fr_curl *h)
{
    char *url = fr_curl_url(h, "/", 1);
    curl_easy_setopt(h->e, CURLOPT_URL, url);
    curl_easy_setopt(h->e, CURLOPT_NOBODY, 1L);
    int rc = fr_curl_perform(h, 0);
    free(url);
    return rc;
}

/* what the server can do, asked once: a feature line of FEAT's reply starts with a space */
static int features(const char *uri, unsigned *out)
{
    int rc;
    fr_curl *h = fr_curl_begin(uri, &rc);
    if (!h) return rc;
    unsigned f = features_get(h);
    if (!(f & F_KNOWN)) {
        fr_curl_quote(h, "*FEAT");
        rc = commands(h);
        if (!rc) {
            f |= F_KNOWN;
            for (const char *p = h->reply.v ? h->reply.v : ""; (p = strstr(p, "\n ")); p += 2) {
                if (!strncasecmp(p + 2, "MLST", 4)) f |= F_MLST;
                else if (!strncasecmp(p + 2, "MFMT", 4)) f |= F_MFMT;
            }
            features_add(h, f);
        }
    }
    fr_curl_end(h);
    *out = f;
    return rc;
}

static int command(const char *uri, const char *a, const char *b)
{
    int rc;
    fr_curl *h = fr_curl_begin(uri, &rc);
    if (!h) return rc;
    fr_curl_quote(h, a);
    if (b) fr_curl_quote(h, b);
    rc = commands(h);
    fr_curl_end(h);
    return rc;
}

static int list_path(const char *uri, const char *path, unsigned f, fr_entries *out);

/* whether a link (or anything) is a folder: the server lets ferret change into it */
static int is_folder(const char *uri, const char *path)
{
    int rc;
    fr_curl *h = fr_curl_begin(uri, &rc);
    if (!h) return 0;
    char *url = fr_curl_url(h, path, 1);
    curl_easy_setopt(h->e, CURLOPT_URL, url);
    curl_easy_setopt(h->e, CURLOPT_NOBODY, 1L);
    rc = fr_curl_perform(h, 0);
    free(url);
    fr_curl_end(h);
    return !rc;
}

static char *child_path(const char *dir, const char *name)
{
    return fr_format("%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}

static int ftp_stat_path(const char *uri, const char *path, unsigned f, fr_entry *out)
{
    if (!strcmp(path, "/")) {
        *out = (fr_entry){.name = fr_strdup("/"), .is_dir = 1};
        return 0;
    }
    if (!quotable(path)) return -EINVAL;
    const char *slash = strrchr(path, '/');
    int rc;
    if (f & F_MLST) {
        fr_curl *h = fr_curl_begin(uri, &rc);
        if (!h) return rc;
        char *cmd = fr_format("MLST %s", path);
        fr_curl_quote(h, cmd);
        free(cmd);
        rc = commands(h);
        if (!rc) {
            /* the facts follow the "250-" that opens MLST's reply; the banner before it can have lines like them */
            const char *r = h->reply.v ? h->reply.v : "", *start = NULL;
            for (const char *p = r; (p = strstr(p, "250-")); p += 4)
                if (p == r || p[-1] == '\n') start = p;
            rc = start ? fr_ftp_mlst_reply(start, out) : -EPROTO;
        }
        fr_curl_end(h);
        if (rc) return rc;
        out->name = fr_strdup(slash + 1);
        out->hidden = slash[1] == '.';
    } else {
        /* LIST only: the parent's listing has it */
        char *parent = slash == path ? fr_strdup("/") : fr_strndup(path, (size_t)(slash - path));
        fr_entries es = {0};
        rc = list_path(uri, parent, f, &es);
        free(parent);
        if (rc) return rc;
        rc = -ENOENT;
        for (int i = 0; i < es.n && rc; i++)
            if (!strcmp(es.v[i].name, slash + 1)) {
                *out = es.v[i];
                es.v[i] = es.v[--es.n];
                rc = 0;
            }
        fr_entries_free(&es);
        if (rc) return rc;
    }
    if (out->is_link) out->is_dir = is_folder(uri, path);
    return 0;
}

static int list_path(const char *uri, const char *path, unsigned f, fr_entries *out)
{
    int rc;
    fr_curl *h = fr_curl_begin(uri, &rc);
    if (!h) return rc;
    int mlsd = (f & F_MLST) && !(features_get(h) & F_NO_MLSD);
    char *url = fr_curl_url(h, path, 1);
    curl_easy_setopt(h->e, CURLOPT_URL, url);
    free(url);
    curl_easy_setopt(h->e, CURLOPT_CUSTOMREQUEST, mlsd ? "MLSD" : "LIST -a");
    rc = fr_curl_perform(h, 0);
    if (rc && mlsd && h->status >= 500 && h->status <= 504) {
        /* FEAT promised MLST, but MLSD is refused */
        features_add(h, F_NO_MLSD);
        mlsd = 0;
        curl_easy_setopt(h->e, CURLOPT_CUSTOMREQUEST, "LIST -a");
        rc = fr_curl_perform(h, 0);
    }
    if (!rc) fr_ftp_listing(h->body.v, h->body.n, mlsd, (int64_t)time(NULL), out);
    fr_curl_end(h);
    return rc;
}

static int ftp_list(const char *uri, fr_entries *out)
{
    unsigned f;
    int rc = features(uri, &f);
    if (rc) return rc;
    const char *path = fr_uri_path(uri);
    fr_entries got = {0};
    rc = list_path(uri, path, f, &got);
    if (rc == -EACCES || rc == -ENOENT) {
        /* the folder could not be entered: a missing one and a forbidden one answer alike */
        fr_entry e = {0};
        int s = ftp_stat_path(uri, path, f, &e);
        rc = s ? s : e.is_dir ? -EACCES : -ENOTDIR;
        fr_entry_free(&e);
    }
    for (int i = 0; !rc && i < got.n; i++)
        if (got.v[i].is_link) {
            char *child = child_path(path, got.v[i].name);
            got.v[i].is_dir = is_folder(uri, child);
            free(child);
        }
    if (rc) fr_entries_free(&got);
    else *out = got;
    return rc;
}

static int ftp_stat(const char *uri, fr_entry *out)
{
    unsigned f;
    int rc = features(uri, &f);
    return rc ? rc : ftp_stat_path(uri, fr_uri_path(uri), f, out);
}

static int exists(const char *uri, const char *path)
{
    unsigned f;
    fr_entry e = {0};
    int rc = features(uri, &f);
    if (!rc) rc = ftp_stat_path(uri, path, f, &e);
    fr_entry_free(&e);
    return rc;
}

static int ftp_mkdir(const char *uri)
{
    const char *path = fr_uri_path(uri);
    if (!quotable(path) || !strcmp(path, "/")) return !strcmp(path, "/") ? -EEXIST : -EINVAL;
    char *cmd = fr_format("MKD %s", path);
    int rc = command(uri, cmd, NULL);
    free(cmd);
    if (rc && rc != -FR_EAUTH && rc != -FR_EUNTRUSTED && !fr_net_broken(rc)) {
        if (!exists(uri, path)) return -EEXIST;
        char *parent = fr_uri_parent(uri);
        int p = parent ? exists(parent, fr_uri_path(parent)) : 0;
        free(parent);
        rc = p == -ENOENT ? -ENOENT : -EACCES;
    }
    return rc;
}

static int ftp_remove(const char *uri)
{
    const char *path = fr_uri_path(uri);
    if (!quotable(path)) return -EINVAL;
    unsigned f;
    fr_entry e = {0};
    int rc = features(uri, &f);
    if (!rc) rc = ftp_stat_path(uri, path, f, &e);
    if (rc) return rc;
    int dir = e.is_dir && !e.is_link;
    fr_entry_free(&e);
    char *cmd = fr_format("%s %s", dir ? "RMD" : "DELE", path);
    rc = command(uri, cmd, NULL);
    free(cmd);
    if (rc && dir && rc != -FR_EAUTH && !fr_net_broken(rc)) {
        fr_entries es = {0};
        if (!list_path(uri, path, f, &es) && es.n) rc = -ENOTEMPTY;
        else if (rc == -ENOENT) rc = -EACCES;
        fr_entries_free(&es);
    } else if (rc == -ENOENT) rc = -EACCES; /* it was there a moment ago */
    return rc;
}

static int ftp_rename(const char *from, const char *to)
{
    if (!fr_uri_same_backend(from, to)) return -EXDEV;
    const char *a = fr_uri_path(from), *b = fr_uri_path(to);
    if (!quotable(a) || !quotable(b)) return -EINVAL;
    int rc = exists(to, b);
    if (!rc) return -EEXIST;
    if (rc != -ENOENT) return rc;
    char *rnfr = fr_format("RNFR %s", a), *rnto = fr_format("RNTO %s", b);
    rc = command(from, rnfr, rnto);
    free(rnfr), free(rnto);
    return rc;
}

static int ftp_set_mtime(const char *uri, int64_t mtime)
{
    const char *path = fr_uri_path(uri);
    if (!quotable(path)) return -EINVAL;
    unsigned f;
    int rc = features(uri, &f);
    if (rc) return rc;
    time_t t = (time_t)mtime;
    struct tm tm;
    gmtime_r(&t, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", &tm);
    /* MFMT (draft-somers-ftp-mfxx), else the MDTM with a time that vsftpd and others take */
    char *cmd = fr_format("%s %s %s", f & F_MFMT ? "MFMT" : "MDTM", stamp, path);
    rc = command(uri, cmd, NULL);
    free(cmd);
    if (rc && !(f & F_MFMT) && rc != -FR_EAUTH && !fr_net_broken(rc)) rc = -ENOTSUP;
    return rc;
}

static int ftp_set_mode(const char *uri, uint32_t mode)
{
    const char *path = fr_uri_path(uri);
    if (!quotable(path)) return -EINVAL;
    char *cmd = fr_format("SITE CHMOD %04o %s", mode & 07777, path);
    int rc = command(uri, cmd, NULL);
    free(cmd);
    return rc;
}

static fr_stream *ftp_open_read(const char *uri, int *err)
{
    fr_curl *h = fr_curl_begin(uri, err);
    if (!h) return NULL;
    char *url = fr_curl_url(h, fr_uri_path(uri), 0);
    curl_easy_setopt(h->e, CURLOPT_URL, url);
    free(url);
    return fr_curl_stream(h, 0, NULL, err);
}

static fr_stream *ftp_open_write(const char *uri, int exclusive, int *err)
{
    const char *path = fr_uri_path(uri);
    if (exclusive) {
        /* FTP has no create-only STOR (STOU picks its own name): a check first, which another client could race */
        int rc = exists(uri, path);
        if (!rc || rc != -ENOENT) return *err = rc ? rc : -EEXIST, NULL;
    }
    fr_curl *h = fr_curl_begin(uri, err);
    if (!h) return NULL;
    char *url = fr_curl_url(h, path, 0);
    curl_easy_setopt(h->e, CURLOPT_URL, url);
    free(url);
    return fr_curl_stream(h, 1, NULL, err);
}

#define FTP_BACKEND(name)                                                                                              \
    {                                                                                                                  \
        .scheme = name, .list = ftp_list, .stat = ftp_stat, .open_read = ftp_open_read, .open_write = ftp_open_write, \
        .mkdir = ftp_mkdir, .rename = ftp_rename, .remove = ftp_remove, .set_mtime = ftp_set_mtime,                    \
        .set_mode = ftp_set_mode,                                                                                      \
    }

const fr_backend fr_backend_ftp = FTP_BACKEND("ftp");
const fr_backend fr_backend_ftps = FTP_BACKEND("ftps");

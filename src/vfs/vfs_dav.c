/* dav://[user@]host[:port]/path and davs:// (WebDAV, RFC 4918, over http and https) through libcurl. Folders are
 * listed with PROPFIND and parsed with expat (remote_list.c); free space is RFC 4331's quota where the server keeps
 * one. A folder's URL ends in "/" for MKCOL, DELETE and MOVE, which servers such as nginx insist on. Modification
 * times cannot be set: getlastmodified is a property the server keeps. */
#define _GNU_SOURCE
#include "remote_list.h"
#include "vfs_curl.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char props[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:propfind xmlns:D=\"DAV:\"><D:prop>"
                            "<D:resourcetype/><D:getcontentlength/><D:getlastmodified/></D:prop></D:propfind>";
static const char quota_props[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?><D:propfind xmlns:D=\"DAV:\"><D:prop>"
                                  "<D:quota-available-bytes/><D:quota-used-bytes/></D:prop></D:propfind>";

static void request(fr_curl *h, const char *method, const char *path, int dir)
{
    char *url = fr_curl_url(h, path, dir);
    curl_easy_setopt(h->e, CURLOPT_URL, url);
    free(url);
    curl_easy_setopt(h->e, CURLOPT_CUSTOMREQUEST, method);
}

static int propfind(const char *uri, int depth, const char *body, fr_entries *children, fr_entry *self, int64_t *avail,
                    int64_t *used)
{
    int rc;
    fr_curl *h = fr_curl_begin(uri, &rc);
    if (!h) return rc;
    const char *path = fr_uri_path(uri);
    request(h, "PROPFIND", path, 0);
    fr_curl_header(h, depth ? "Depth: 1" : "Depth: 0");
    fr_curl_header(h, "Content-Type: application/xml; charset=utf-8");
    curl_easy_setopt(h->e, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h->e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h->e, CURLOPT_MAXREDIRS, 3L);
    rc = fr_curl_perform(h, 0);
    if (!rc && h->status != 207) rc = -EPROTONOSUPPORT; /* a plain web server, not WebDAV */
    if (!rc) rc = fr_dav_multistatus(h->body.v ? h->body.v : "", h->body.n, path, children, self, avail, used);
    fr_curl_end(h);
    return rc;
}

static int dav_stat(const char *uri, fr_entry *out) { return propfind(uri, 0, props, NULL, out, NULL, NULL); }

static int dav_list(const char *uri, fr_entries *out)
{
    fr_entries got = {0};
    fr_entry self = {0};
    int rc = propfind(uri, 1, props, &got, &self, NULL, NULL);
    if (!rc && !self.is_dir) rc = -ENOTDIR;
    fr_entry_free(&self);
    if (rc) fr_entries_free(&got);
    else *out = got;
    return rc;
}

/* a method without a body: its HTTP status, mapped, with the exceptions given */
static int simple(const char *uri, const char *method, const char *path, int dir, const char *h1, const char *h2,
                  long conflict, int conflict_rc)
{
    int rc;
    fr_curl *h = fr_curl_begin(uri, &rc);
    if (!h) return rc;
    request(h, method, path, dir);
    if (h1) fr_curl_header(h, h1);
    if (h2) fr_curl_header(h, h2);
    rc = fr_curl_perform(h, 1);
    if (!rc && h->status == conflict) rc = conflict_rc;
    else if (!rc) rc = fr_http_errno(h->status);
    fr_curl_end(h);
    return rc;
}

static int dav_mkdir(const char *uri)
{
    const char *path = fr_uri_path(uri);
    if (!strcmp(path, "/")) return -EEXIST;
    /* RFC 4918 9.3.1: 405 when something is there already */
    return simple(uri, "MKCOL", path, 1, NULL, NULL, 405, -EEXIST);
}

static int dav_remove(const char *uri)
{
    fr_entries kids = {0};
    fr_entry self = {0};
    int rc = propfind(uri, 1, props, &kids, &self, NULL, NULL);
    int dir = self.is_dir, n = kids.n;
    fr_entry_free(&self);
    fr_entries_free(&kids);
    if (rc) return rc;
    /* DELETE takes a collection with everything in it (RFC 4918 9.6.1); remove() is for an empty one */
    if (dir && n) return -ENOTEMPTY;
    return simple(uri, "DELETE", fr_uri_path(uri), dir, NULL, NULL, 0, 0);
}

static int dav_rename(const char *from, const char *to)
{
    if (!fr_uri_same_backend(from, to)) return -EXDEV;
    fr_entry e = {0};
    int rc = dav_stat(from, &e);
    int dir = e.is_dir;
    fr_entry_free(&e);
    if (rc) return rc;
    rc = dav_stat(to, &e);
    fr_entry_free(&e);
    if (!rc) return -EEXIST;
    if (rc != -ENOENT) return rc;
    fr_curl *h = fr_curl_begin(from, &rc);
    if (!h) return rc;
    char *dest = fr_curl_url(h, fr_uri_path(to), dir), *line = fr_format("Destination: %s", dest);
    fr_curl_end(h);
    rc = simple(from, "MOVE", fr_uri_path(from), dir, line, "Overwrite: F", 0, 0);
    free(dest), free(line);
    return rc;
}

static int dav_free_space(const char *uri, uint64_t *free_bytes, uint64_t *total_bytes)
{
    int64_t avail, used;
    fr_entry self = {0};
    int rc = propfind(uri, 0, quota_props, NULL, &self, &avail, &used);
    fr_entry_free(&self);
    if (rc) return rc;
    if (avail < 0) return -ENOTSUP;
    *free_bytes = (uint64_t)avail;
    *total_bytes = (uint64_t)avail + (used > 0 ? (uint64_t)used : 0);
    return 0;
}

static fr_stream *dav_open_read(const char *uri, int *err)
{
    fr_curl *h = fr_curl_begin(uri, err);
    if (!h) return NULL;
    request(h, "GET", fr_uri_path(uri), 0);
    curl_easy_setopt(h->e, CURLOPT_CUSTOMREQUEST, NULL);
    curl_easy_setopt(h->e, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h->e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h->e, CURLOPT_MAXREDIRS, 3L);
    return fr_curl_stream(h, 0, NULL, err);
}

static fr_stream *dav_open_write(const char *uri, int exclusive, int *err)
{
    if (exclusive) {
        /* If-None-Match: * below makes it atomic on servers that honour it on PUT; the check covers the rest */
        fr_entry e = {0};
        int rc = dav_stat(uri, &e);
        fr_entry_free(&e);
        if (rc != -ENOENT) return *err = rc ? rc : -EEXIST, NULL;
    }
    fr_curl *h = fr_curl_begin(uri, err);
    if (!h) return NULL;
    request(h, "PUT", fr_uri_path(uri), 0);
    curl_easy_setopt(h->e, CURLOPT_CUSTOMREQUEST, NULL);
    fr_curl_header(h, "Transfer-Encoding: chunked");
    if (exclusive) fr_curl_header(h, "If-None-Match: *");
    return fr_curl_stream(h, 1, NULL, err);
}

#define DAV_BACKEND(name)                                                                                              \
    {                                                                                                                  \
        .scheme = name, .list = dav_list, .stat = dav_stat, .open_read = dav_open_read, .open_write = dav_open_write, \
        .mkdir = dav_mkdir, .rename = dav_rename, .remove = dav_remove, .free_space = dav_free_space,                  \
    }

const fr_backend fr_backend_dav = DAV_BACKEND("dav");
const fr_backend fr_backend_davs = DAV_BACKEND("davs");

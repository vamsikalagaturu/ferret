/* What FTP and WebDAV servers answer, parsed (src/vfs/remote_list.c), and the error mapping of their codes. */
#define _GNU_SOURCE
#include "check.h"
#include "net/net.h"
#include "vfs/remote_list.h"
#include "vfs/vfs_curl.h"
#include <errno.h>
#include <time.h>

static int64_t utc(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s};
    return (int64_t)timegm(&tm);
}

static fr_entry *find(fr_entries *es, const char *name)
{
    for (int i = 0; i < es->n; i++)
        if (!strcmp(es->v[i].name, name)) return &es->v[i];
    return NULL;
}

static void mlsd(void)
{
    static const char listing[] = "type=cdir;modify=20261115031200;perm=el; .\r\n"
                                  "type=pdir;modify=20261115031200;perm=el; ..\r\n"
                                  "type=file;size=5;modify=20010909014640;UNIX.mode=0644; a b.txt\r\n"
                                  "Type=dir;Modify=20261101120000.123;unix.mode=0755;sizd=4096; sub\r\n"
                                  "type=OS.unix=slink:/etc;modify=20260101000000; link\r\n"
                                  "type=file;size=0; .hidden\r\n"
                                  "garbage\r\n";
    fr_entries es = {0};
    fr_ftp_listing(listing, sizeof listing - 1, 1, 0, &es);
    CHECK(es.n == 4);
    fr_entry *a = find(&es, "a b.txt"), *sub = find(&es, "sub"), *link = find(&es, "link"), *h = find(&es, ".hidden");
    CHECK(a && !a->is_dir && a->size == 5 && a->mtime == 1000000000 && a->mode == 0644);
    CHECK(sub && sub->is_dir && sub->size == 0 && sub->mode == 0755 && sub->mtime == utc(2026, 11, 1, 12, 0, 0));
    CHECK(link && link->is_link);
    CHECK(h && h->hidden);
    fr_entries_free(&es);

    fr_entry e = {0};
    CHECK(fr_ftp_mlst_reply("250-Listing /x\r\n type=file;size=42;modify=20010909014640; /x\r\n250 End\r\n", &e) == 0);
    CHECK(e.size == 42 && e.mtime == 1000000000 && !e.is_dir && !e.name);
    CHECK(fr_ftp_mlst_reply("250 nothing\r\n", &e) == -ENOENT);
}

static void list(void)
{
    int64_t now = utc(2026, 11, 15, 4, 0, 0);
    static const char unix_ls[] = "total 12\r\n"
                                  "drwxr-xr-x    2 1000     1000         4096 Nov 15 03:12 a dir\r\n"
                                  "-rw-r--r--    1 ftp      ftp             5 Sep  9  2001 old.txt\r\n"
                                  "-rwsr-x--T    1 owner         1234567 Dec 24 23:59 lastyear\r\n"
                                  "lrwxrwxrwx    1 0        0               4 Jan  2 10:00 l -> /etc\r\n"
                                  "drwxr-xr-x    2 0        0            4096 Nov 15 03:12 .\r\n";
    fr_entries es = {0};
    fr_ftp_listing(unix_ls, sizeof unix_ls - 1, 0, now, &es);
    CHECK(es.n == 4);
    fr_entry *d = find(&es, "a dir"), *o = find(&es, "old.txt"), *y = find(&es, "lastyear"), *l = find(&es, "l");
    CHECK(d && d->is_dir && d->mode == 0755 && d->mtime == utc(2026, 11, 15, 3, 12, 0));
    CHECK(o && o->size == 5 && o->mtime == utc(2001, 9, 9, 0, 0, 0) && o->mode == 0644);
    /* no group column; a date ahead of now is last year's */
    CHECK(y && y->size == 1234567 && y->mtime == utc(2025, 12, 24, 23, 59, 0) && y->mode == (04000 | 0750 | 01000));
    CHECK(l && l->is_link);
    fr_entries_free(&es);

    static const char dos[] = "11-15-26  03:12AM       <DIR>          My Folder\r\n"
                              "01-02-99  11:30PM                 1024 report.doc\r\n";
    fr_ftp_listing(dos, sizeof dos - 1, 0, now, &es);
    CHECK(es.n == 2);
    fr_entry *f = find(&es, "My Folder"), *r = find(&es, "report.doc");
    CHECK(f && f->is_dir && f->mtime == utc(2026, 11, 15, 3, 12, 0));
    CHECK(r && r->size == 1024 && r->mtime == utc(1999, 1, 2, 23, 30, 0));
    fr_entries_free(&es);
}

static void dav(void)
{
    static const char xml[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<d:multistatus xmlns:d=\"DAV:\" xmlns:x=\"urn:other\">"
        "<d:response><d:href>/dav/a%20b/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype>"
        "<d:quota-available-bytes>1000</d:quota-available-bytes><d:quota-used-bytes>24</d:quota-used-bytes>"
        "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "<d:response><d:href>http://host:8080/dav/a%20b/f%C3%BC.txt</d:href><d:propstat><d:prop><d:resourcetype/>"
        "<d:getcontentlength>12</d:getcontentlength><d:getlastmodified>Sun, 09 Sep 2001 01:46:40 GMT</d:getlastmodified>"
        "<x:other>ignored</x:other></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
        "<d:propstat><d:prop><d:getcontentlength>99</d:getcontentlength></d:prop>"
        "<d:status>HTTP/1.1 404 Not Found</d:status></d:propstat></d:response>"
        "<D:response xmlns:D=\"DAV:\"><D:href>/dav/a%20b/sub/</D:href><D:propstat><D:prop><D:resourcetype>"
        "<D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
        "<d:response><d:href>/dav/a%20b/gone</d:href><d:status>HTTP/1.1 404 Not Found</d:status></d:response>"
        "</d:multistatus>";
    fr_entries es = {0};
    fr_entry self = {0};
    int64_t avail, used;
    CHECK(fr_dav_multistatus(xml, sizeof xml - 1, "/dav/a b", &es, &self, &avail, &used) == 0);
    CHECK(self.is_dir && !strcmp(self.name, "a b"));
    CHECK(avail == 1000 && used == 24);
    CHECK(es.n == 2);
    fr_entry *f = find(&es, "f\xc3\xbc.txt"), *sub = find(&es, "sub");
    CHECK(f && !f->is_dir && f->size == 12 && f->mtime == 1000000000);
    CHECK(sub && sub->is_dir);
    fr_entries_free(&es);
    fr_entry_free(&self);

    CHECK(fr_dav_multistatus(xml, sizeof xml - 1, "/elsewhere", NULL, &self, NULL, NULL) == -ENOENT);
    CHECK(fr_dav_multistatus("<html>not dav</html>", 20, "/", &es, NULL, NULL, NULL) == -EPROTO);
    CHECK(fr_dav_multistatus("<d:multistatus xmlns:d=\"DAV:\"><unclosed>", 40, "/", &es, NULL, NULL, NULL) == -EPROTO);
    CHECK(es.n == 0);

    static const char root[] = "<multistatus xmlns=\"DAV:\"><response><href>/</href><propstat><prop><resourcetype>"
                               "<collection/></resourcetype></prop><status>HTTP/1.1 200 OK</status></propstat>"
                               "</response></multistatus>";
    CHECK(fr_dav_multistatus(root, sizeof root - 1, "/", NULL, &self, NULL, NULL) == 0);
    CHECK(self.is_dir && !strcmp(self.name, "/") && !self.hidden);
    fr_entry_free(&self);
}

static void errors(void)
{
    CHECK(fr_http_errno(207) == 0);
    CHECK(fr_http_errno(401) == -FR_EAUTH);
    CHECK(fr_http_errno(403) == -EACCES);
    CHECK(fr_http_errno(404) == -ENOENT);
    CHECK(fr_http_errno(409) == -ENOENT);
    CHECK(fr_http_errno(412) == -EEXIST);
    CHECK(fr_http_errno(507) == -ENOSPC);
    CHECK(fr_http_errno(500) == -EIO);
    CHECK(fr_ftp_errno(550) == -ENOENT);
    CHECK(fr_ftp_errno(553) == -EACCES);
    CHECK(fr_ftp_errno(552) == -ENOSPC);
    CHECK(fr_ftp_errno(530) == -FR_EAUTH);
    CHECK(fr_ftp_errno(502) == -ENOTSUP);
    CHECK(fr_ftp_errno(250) == 0);

    char *p = fr_url_path("/a b/%c/ü#?");
    CHECK_STR(p, "/a%20b/%25c/%C3%BC%23%3F");
    free(p);
    p = fr_url_unescape("a%20b%2fc%zz%4", 14);
    CHECK_STR(p, "a b/c%zz%4");
    free(p);
}

static void uris(void)
{
    fr_net_uri u;
    CHECK(fr_net_parse("ftps://bob@nas/share", &u) == 0 && u.port == 21);
    fr_net_uri_free(&u);
    CHECK(fr_net_parse("davs://nas/dav", &u) == 0 && u.port == 443);
    fr_net_uri_free(&u);
    CHECK(fr_net_parse("nfs://nas/srv/nfs", &u) == 0 && u.port == 2049 && !strcmp(u.path, "/srv/nfs"));
    fr_net_uri_free(&u);
    char *n = fr_net_normalize("FTPS://Nas:990/x/");
    CHECK_STR(n, "ftps://nas:990/x");
    free(n);
    n = fr_net_normalize("dav://nas:80/");
    CHECK_STR(n, "dav://nas/");
    free(n);
    CHECK(fr_vfs_backend("ftp://h/") == &fr_backend_ftp && fr_vfs_backend("ftps://h/") == &fr_backend_ftps);
    CHECK(fr_vfs_backend("dav://h/") == &fr_backend_dav && fr_vfs_backend("davs://h/") == &fr_backend_davs);
    CHECK(fr_vfs_backend("nfs://h/") == &fr_backend_nfs);

    /* curl URLs: FTP paths are absolute on the server, folders end in "/" */
    int err = 0;
    fr_curl *h = fr_curl_begin("ftp://127.0.0.1:1/a b/c", &err);
    CHECK(h != NULL);
    if (h) {
        char *url = fr_curl_url(h, "/a b/c", 1);
        CHECK_STR(url, "ftp://127.0.0.1:1/%2Fa%20b/c/");
        free(url);
        url = fr_curl_url(h, "/", 1);
        CHECK_STR(url, "ftp://127.0.0.1:1/%2F");
        free(url);
        fr_curl_end(h);
    }
    h = fr_curl_begin("davs://[::1]/x", &err);
    CHECK(h != NULL);
    if (h) {
        char *url = fr_curl_url(h, "/x", 1);
        CHECK_STR(url, "https://[::1]:443/x/");
        free(url);
        fr_curl_end(h);
    }
    /* a user named without a password asks for one before connecting */
    CHECK(fr_curl_begin("ftp://bob@127.0.0.1:1/", &err) == NULL && err == -FR_EAUTH);
    /* nothing listens on port 1 */
    fr_entries es = {0};
    CHECK(fr_vfs_list("ftp://127.0.0.1:1/", &es) == -ECONNREFUSED);
    CHECK(fr_vfs_list("dav://127.0.0.1:1/", &es) == -ECONNREFUSED);
    fr_conn_close_all();
}

int main(void)
{
    mlsd();
    list();
    dav();
    errors();
    uris();
    return CHECK_DONE();
}

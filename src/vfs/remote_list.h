#ifndef FR_REMOTE_LIST_H
#define FR_REMOTE_LIST_H
/* What FTP and WebDAV servers answer, made fr_entry: FTP listings (MLSD, RFC 3659, or LIST in the formats Unix ls and
 * Windows servers write), MLST replies, WebDAV multistatus bodies (RFC 4918) and HTTP statuses. No network here, so
 * the tests feed it canned answers. */
#include "vfs.h"
#include <stddef.h>
#include <stdint.h>

/* one line of an MLSD listing ("type=file;size=5;modify=20260101120000; name"): 1 and *e filled, 0 for a line
 * that is no entry (".", "..", garbage) */
int fr_ftp_mlsd_line(const char *line, size_t n, fr_entry *e);
/* one line of a LIST listing, "-rw-r--r-- 1 u g 5 Nov 15 03:12 name" or "11-15-26  03:12AM  <DIR>  name"; now
 * dates the lines that leave out the year */
int fr_ftp_list_line(const char *line, size_t n, int64_t now, fr_entry *e);
/* a whole listing, CRLF or LF lines, appended to out */
void fr_ftp_listing(const char *buf, size_t n, int mlsd, int64_t now, fr_entries *out);
/* the server's reply to MLST (the facts line starts with a space): 0, or -ENOENT when it has none */
int fr_ftp_mlst_reply(const char *reply, fr_entry *e);

/* A PROPFIND answer for self (the path asked for, unescaped): its children go to *children (may be NULL), self
 * to *self (may be NULL; -ENOENT when the answer lacks it) and RFC 4331's quota to avail/used (-1 where absent).
 * 0, or -EPROTO for a body that is no multistatus. */
int fr_dav_multistatus(const char *xml, size_t n, const char *self, fr_entries *children, fr_entry *self_out,
                       int64_t *avail, int64_t *used);
/* an HTTP status as -errno: 0 for 2xx, -FR_EAUTH for 401, -ENOENT for 404 ... */
int fr_http_errno(long status);

/* a path made a URL's: every byte but unreserved ones and "/" as %XX */
char *fr_url_path(const char *path);
/* %XX undone in n bytes of s */
char *fr_url_unescape(const char *s, size_t n);
#endif

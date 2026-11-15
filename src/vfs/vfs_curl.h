#ifndef FR_VFS_CURL_H
#define FR_VFS_CURL_H
/* What the FTP and WebDAV backends share over libcurl: a pooled place per server (fr_conn) that keeps idle curl
 * handles, each with its own multi handle, so its connection (an FTP control connection, an HTTP keep-alive one)
 * stays open between calls; one handle per call or stream, so calls on other threads get connections of their own.
 * TLS certificates are checked against the system's CA certificates; one that does not verify can be trusted on
 * first use (fr_trust_ask), its SHA-256 fingerprint kept in ferret's known_certs. */
#include "net/net.h"
#include "vfs.h"
#include <curl/curl.h>

typedef enum { FR_CURL_FTP, FR_CURL_DAV } fr_curl_proto;

/* what a backend learned about a server, shared by the threads that call it (__atomic loads and stores) */
typedef struct {
    unsigned features;
    long auth; /* HTTP: the CURLAUTH_* to send credentials with */
} fr_curl_server;

typedef struct {
    char *v;
    size_t n, cap;
} fr_curl_buf;

typedef struct fr_curl fr_curl;
struct fr_curl {
    CURL *e;
    fr_curl_proto proto;
    fr_curl_server *srv;
    fr_net_uri u;
    char *key;  /* the server's, for passwords and trust: "ftp://bob@host:21" */
    long status; /* the last FTP reply code or HTTP status */
    fr_curl_buf body, reply; /* what a call got: the transfer's data, the FTP replies or HTTP headers */
    struct curl_slist *headers, *quote;
    /* private to vfs_curl.c */
    CURLM *m;
    fr_conn *c;
    void *stream;
    int tls, had_creds, has_pin, pin_mismatch, seen, verify_error;
    char pin[100], fingerprint[100];
};

void fr_curl_buf_add(fr_curl_buf *b, const void *p, size_t n);

/* A handle for uri's server with the common options set (credentials, TLS, timeouts, collecting the body and
 * replies); NULL with *err: -EINVAL for an address that is not one, -FR_EAUTH when the address names a user ferret
 * has no password for (FTP). */
fr_curl *fr_curl_begin(const char *uri, int *err);
/* the URL of path on h's server; dir adds a trailing "/" */
char *fr_curl_url(const fr_curl *h, const char *path, int dir);
void fr_curl_header(fr_curl *h, const char *line);
void fr_curl_quote(fr_curl *h, const char *command);
/* runs what h was set up for, once more on a new connection when a reused one turned out to be gone: 0 or -errno;
 * -FR_EAUTH for refused credentials, -FR_EUNTRUSTED for a certificate waiting for the user. HTTP statuses of 300 and
 * up are errors (fr_http_errno), unless http_any. */
int fr_curl_perform(fr_curl *h, int http_any);
/* back to the server's idle handles */
void fr_curl_end(fr_curl *h);

/* a stream over what h was set up for (a download, or an upload when upload), started: it fails here when the
 * server refuses it. It owns h. finish (may be NULL) runs when an upload ended, on its status: its -errno or 0. */
fr_stream *fr_curl_stream(fr_curl *h, int upload, int (*finish)(fr_curl *h, int rc), int *err);

/* FTP reply codes as -errno, for the commands where they say more than curl's error */
int fr_ftp_errno(long code);
#endif

#define _GNU_SOURCE
#include "vfs_curl.h"
#include "remote_list.h"
#include <dirent.h>
#include <errno.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONNECT_TIMEOUT_MS 10000
#define STALL_S 30
#define IDLE_MAX 4
#define READ_AHEAD (1 << 20)

typedef struct {
    fr_curl_server pub;
    pthread_mutex_t lock;
    struct {
        fr_curl **v;
        int n, cap;
    } idle;
} server;

typedef struct {
    fr_stream s;
    fr_curl *h;
    int upload, done, paused, eof, started;
    CURLcode res;
    size_t off;
    const char *up;
    size_t up_n;
    int (*finish)(fr_curl *h, int rc);
} cstream;

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int ssl_index = -1, openssl_curl;
static const char *ca_file; /* when libcurl was built without a default */
static fr_curl_buf ca_blob; /* Android's CA certificates, which OpenSSL cannot look up in their folder */

void fr_curl_buf_add(fr_curl_buf *b, const void *p, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        b->v = fr_realloc(b->v, cap);
        b->cap = cap;
    }
    memcpy(b->v + b->n, p, n);
    b->n += n;
    b->v[b->n] = 0;
}

#ifdef __ANDROID__
static void load_android_cas(void)
{
    /* the updatable store (Android 14+) first, then the one in the system image; the files are PEM, named by
     * OpenSSL's old subject hash, so they are read in rather than given to OpenSSL as a CA path */
    static const char *const dirs[] = {"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts"};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs && !ca_blob.n; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char *path = fr_format("%s/%s", dirs[i], de->d_name);
            FILE *f = fopen(path, "rb");
            free(path);
            if (!f) continue;
            char buf[8192];
            size_t r;
            while ((r = fread(buf, 1, sizeof buf, f)) > 0) fr_curl_buf_add(&ca_blob, buf, r);
            fr_curl_buf_add(&ca_blob, "\n", 1);
            fclose(f);
        }
        closedir(d);
    }
}
#endif

static void global_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    openssl_curl = v->ssl_version && !strncmp(v->ssl_version, "OpenSSL", 7);
    ssl_index = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
#ifdef __ANDROID__
    load_android_cas();
#else
    CURL *e = curl_easy_init();
    char *file = NULL, *dir = NULL;
    if (e) {
        curl_easy_getinfo(e, CURLINFO_CAINFO, &file);
        curl_easy_getinfo(e, CURLINFO_CAPATH, &dir);
    }
    if (!file && !dir) {
        static const char *const bundles[] = {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                              "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem"};
        for (size_t i = 0; i < sizeof bundles / sizeof *bundles && !ca_file; i++)
            if (!access(bundles[i], R_OK)) ca_file = bundles[i];
    }
    if (e) curl_easy_cleanup(e);
#endif
}

/* ---- the servers in the pool ---- */

static void destroy(fr_curl *h)
{
    if (h->m) curl_multi_cleanup(h->m);
    if (h->e) curl_easy_cleanup(h->e);
    free(h->body.v), free(h->reply.v);
    free(h);
}

static void *server_open(const char *key, const fr_net_uri *u, int *err)
{
    server *s = fr_realloc(NULL, sizeof *s);
    *s = (server){0};
    pthread_mutex_init(&s->lock, NULL);
    return s;
}

static void server_close(void *p)
{
    server *s = p;
    for (int i = 0; i < s->idle.n; i++) destroy(s->idle.v[i]);
    free(s->idle.v);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

static const fr_conn_ops server_ops = {server_open, server_close};

/* ---- certificates trusted on first use: "host:port AB:CD:..." lines in known_certs ---- */

static char *certs_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg == '/') return fr_format("%s/ferret/known_certs", xdg);
    return home && *home ? fr_format("%s/.config/ferret/known_certs", home) : NULL;
}

static char *host_port(const fr_net_uri *u)
{
    int v6 = strchr(u->host, ':') != NULL;
    return fr_format("%s%s%s:%d", v6 ? "[" : "", u->host, v6 ? "]" : "", u->port);
}

static int pin_load(const char *hostport, char *out, size_t cap)
{
    char *path = certs_path();
    FILE *f = path ? fopen(path, "r") : NULL;
    free(path);
    if (!f) return 0;
    char line[512];
    size_t hn = strlen(hostport);
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        if (strncmp(line, hostport, hn) || line[hn] != ' ') continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(out, cap, "%s", line + hn + 1);
        found = 1;
    }
    fclose(f);
    return found;
}

typedef struct {
    char *hostport, *fingerprint;
} pin;

static void pin_accept(void *p)
{
    pin *w = p;
    char *path = certs_path();
    if (!path) return;
    char *dir = fr_strndup(path, (size_t)(strrchr(path, '/') - path)), *tmp = fr_format("%s.new", path);
    fr_mkdirs(dir);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    size_t hn = strlen(w->hostport);
    if (out) {
        char line[512];
        while (in && fgets(line, sizeof line, in))
            if (strncmp(line, w->hostport, hn) || line[hn] != ' ') fputs(line, out);
        fprintf(out, "%s %s\n", w->hostport, w->fingerprint);
        if (fclose(out) || rename(tmp, path)) {
            fprintf(stderr, "ferret: could not write %s\n", path);
            unlink(tmp);
        }
    }
    if (in) fclose(in);
    free(dir), free(tmp), free(path);
}

static void pin_free(void *p)
{
    pin *w = p;
    free(w->hostport), free(w->fingerprint);
    free(w);
}

static int verify_cb(int ok, X509_STORE_CTX *x)
{
    SSL *ssl = X509_STORE_CTX_get_ex_data(x, SSL_get_ex_data_X509_STORE_CTX_idx());
    fr_curl *h = ssl ? SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), ssl_index) : NULL;
    if (!h) return ok;
    X509 *leaf = X509_STORE_CTX_get0_cert(x);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    if (leaf && !h->seen && X509_digest(leaf, EVP_sha256(), md, &n)) {
        for (unsigned i = 0; i < n && i * 3 + 3 <= sizeof h->fingerprint; i++)
            snprintf(h->fingerprint + i * 3, 4, "%02X%s", md[i], i + 1 < n ? ":" : "");
        h->seen = 1;
    }
    if (h->has_pin) {
        /* a certificate the user trusted stands for the server, whatever the CAs say about it */
        if (h->seen && !strcmp(h->pin, h->fingerprint)) {
            X509_STORE_CTX_set_error(x, X509_V_OK);
            return 1;
        }
        h->pin_mismatch = 1;
        X509_STORE_CTX_set_error(x, X509_V_ERR_CERT_REJECTED);
        return 0;
    }
    if (!ok && !h->verify_error) h->verify_error = X509_STORE_CTX_get_error(x);
    return ok;
}

/* libcurl's OpenSSL leaves failing verification to it after the handshake (SSL_VERIFY_NONE); this callback only
 * watches, and vouches for a pinned certificate */
static CURLcode ssl_ctx_cb(CURL *e, void *ctx, void *h)
{
    SSL_CTX_set_ex_data(ctx, ssl_index, h);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, verify_cb);
    return CURLE_OK;
}

static int untrusted(fr_curl *h)
{
    char *hp = host_port(&h->u), *question;
    if (h->pin_mismatch)
        question = fr_format("The certificate of %s changed since you trusted it. Its SHA-256 fingerprint is now %s. "
                             "Compare it with the server's own (openssl x509 -noout -fingerprint -sha256 -in its "
                             "certificate) before you trust it.", hp, h->fingerprint);
    else
        question = fr_format("ferret cannot verify the certificate of %s: %s. Its SHA-256 fingerprint is %s. Compare it "
                             "with the server's own (openssl x509 -noout -fingerprint -sha256 -in its certificate) "
                             "before you trust it.", hp,
                             h->verify_error ? X509_verify_cert_error_string(h->verify_error) : "it names another server",
                             h->fingerprint);
    pin *w = fr_realloc(NULL, sizeof *w);
    *w = (pin){hp, fr_strdup(h->fingerprint)};
    fr_trust_ask(h->key, question, pin_accept, w, pin_free);
    free(question);
    return -FR_EUNTRUSTED;
}

/* ---- handles ---- */

static size_t body_cb(char *p, size_t size, size_t n, void *ud)
{
    fr_curl *h = ud;
    cstream *st = h->stream;
    if (st && !st->upload && h->body.n - st->off > READ_AHEAD) {
        st->paused = 1;
        return CURL_WRITEFUNC_PAUSE;
    }
    fr_curl_buf_add(&h->body, p, size * n);
    return size * n;
}

static size_t reply_cb(char *p, size_t size, size_t n, void *ud)
{
    fr_curl *h = ud;
    if (h->reply.n < (1 << 20)) fr_curl_buf_add(&h->reply, p, size * n);
    return size * n;
}

fr_curl *fr_curl_begin(const char *uri, int *err)
{
    pthread_once(&once, global_init);
    fr_net_uri u;
    if (fr_net_parse(uri, &u)) return *err = -EINVAL, NULL;
    fr_curl_proto proto = !strncmp(u.scheme, "ftp", 3) ? FR_CURL_FTP : FR_CURL_DAV;
    int tls = !strcmp(u.scheme, "ftps") || !strcmp(u.scheme, "davs");
    char *key = fr_net_key(&u);
    fr_cred cred = {0};
    int have = !fr_creds_get(key, &cred);
    fr_conn *c = NULL;
    /* anonymous FTP is what an address without a user means; one that names a user needs the password */
    if (proto == FR_CURL_FTP && !have && u.user && strcmp(u.user, "anonymous") && strcmp(u.user, "ftp")) *err = -FR_EAUTH;
    else c = fr_conn_get(key, &u, &server_ops, NULL, err);
    if (!c) {
        fr_cred_free(&cred);
        free(key);
        fr_net_uri_free(&u);
        return NULL;
    }
    server *s = fr_conn_session(c);
    fr_conn_unlock(c, 0);
    pthread_mutex_lock(&s->lock);
    fr_curl *h = s->idle.n ? s->idle.v[--s->idle.n] : NULL;
    pthread_mutex_unlock(&s->lock);
    if (h) curl_easy_reset(h->e);
    else {
        h = fr_realloc(NULL, sizeof *h);
        *h = (fr_curl){.e = curl_easy_init(), .m = curl_multi_init()};
        if (!h->e || !h->m) {
            destroy(h);
            fr_conn_unref(c);
            fr_cred_free(&cred);
            free(key);
            fr_net_uri_free(&u);
            return *err = -ENOMEM, NULL;
        }
    }
    h->proto = proto, h->srv = &s->pub, h->u = u, h->key = key, h->c = c, h->tls = tls;
    h->status = 0, h->body.n = h->reply.n = 0, h->headers = h->quote = NULL, h->stream = NULL;
    h->had_creds = have, h->seen = h->verify_error = h->pin_mismatch = 0;
    h->fingerprint[0] = 0;
    CURL *e = h->e;
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, (long)CONNECT_TIMEOUT_MS);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, (long)STALL_S);
    curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, "ferret/" FR_VERSION);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, h);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, reply_cb);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, h);
    if (tls) {
        char *hp = host_port(&u);
        h->has_pin = openssl_curl && pin_load(hp, h->pin, sizeof h->pin);
        free(hp);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, h->has_pin ? 0L : 2L);
        if (openssl_curl) {
            curl_easy_setopt(e, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_cb);
            curl_easy_setopt(e, CURLOPT_SSL_CTX_DATA, h);
        }
        if (ca_blob.n) {
            struct curl_blob blob = {ca_blob.v, ca_blob.n, CURL_BLOB_NOCOPY};
            curl_easy_setopt(e, CURLOPT_CAINFO_BLOB, &blob);
        } else if (ca_file) curl_easy_setopt(e, CURLOPT_CAINFO, ca_file);
    } else h->has_pin = 0;
    const char *user = have && *cred.user ? cred.user : u.user;
    if (user) curl_easy_setopt(e, CURLOPT_USERNAME, user);
    if (have) curl_easy_setopt(e, CURLOPT_PASSWORD, cred.password);
    if (proto == FR_CURL_FTP) {
        curl_easy_setopt(e, CURLOPT_FTP_FILEMETHOD, (long)CURLFTPMETHOD_SINGLECWD);
        curl_easy_setopt(e, CURLOPT_SERVER_RESPONSE_TIMEOUT, (long)STALL_S);
        if (tls && u.port != 990) curl_easy_setopt(e, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    } else if (have) {
        long auth = __atomic_load_n(&s->pub.auth, __ATOMIC_RELAXED);
        curl_easy_setopt(e, CURLOPT_HTTPAUTH, auth ? auth : (long)CURLAUTH_BASIC);
    }
    fr_cred_free(&cred);
    return h;
}

char *fr_curl_url(const fr_curl *h, const char *path, int dir)
{
    const fr_net_uri *u = &h->u;
    const char *scheme = h->proto == FR_CURL_FTP ? (h->tls && u->port == 990 ? "ftps" : "ftp") : h->tls ? "https" : "http";
    int v6 = strchr(u->host, ':') != NULL;
    char *esc = fr_url_path(path), *url;
    size_t n = strlen(esc);
    int slash = dir && n && esc[n - 1] != '/';
    /* %2F: FTP paths are the server's absolute ones, not relative to where the login starts (RFC 1738) */
    if (h->proto == FR_CURL_FTP)
        url = fr_format("%s://%s%s%s:%d/%%2F%s%s", scheme, v6 ? "[" : "", u->host, v6 ? "]" : "", u->port, esc + 1,
                        slash && n > 1 ? "/" : "");
    else url = fr_format("%s://%s%s%s:%d%s%s", scheme, v6 ? "[" : "", u->host, v6 ? "]" : "", u->port, esc, slash ? "/" : "");
    free(esc);
    return url;
}

void fr_curl_header(fr_curl *h, const char *line) { h->headers = curl_slist_append(h->headers, line); }

void fr_curl_quote(fr_curl *h, const char *command) { h->quote = curl_slist_append(h->quote, command); }

void fr_curl_end(fr_curl *h)
{
    curl_slist_free_all(h->headers);
    curl_slist_free_all(h->quote);
    h->headers = h->quote = NULL;
    free(h->key);
    h->key = NULL;
    fr_net_uri_free(&h->u);
    if (h->body.cap > 4 * READ_AHEAD) free(h->body.v), h->body = (fr_curl_buf){0};
    fr_conn *c = h->c;
    server *s = fr_conn_session(c);
    pthread_mutex_lock(&s->lock);
    if (s->idle.n < IDLE_MAX) {
        FR_PUSH(s->idle, h);
        h = NULL;
    }
    pthread_mutex_unlock(&s->lock);
    if (h) destroy(h);
    fr_conn_unref(c);
}

/* ---- errors ---- */

int fr_ftp_errno(long code)
{
    switch (code) {
    case 421:
    case 425:
    case 426: return -ECONNRESET;
    case 450: return -EBUSY;
    case 452:
    case 552: return -ENOSPC;
    case 500:
    case 502:
    case 504: return -ENOTSUP;
    case 501: return -EINVAL;
    case 521: return -EEXIST;
    case 530:
    case 532: return -FR_EAUTH;
    case 550: return -ENOENT;
    case 553: return -EACCES;
    default: return 0;
    }
}

static int curl_errno(CURLcode res)
{
    switch (res) {
    case CURLE_OK: return 0;
    case CURLE_UNSUPPORTED_PROTOCOL: return -EPROTONOSUPPORT;
    case CURLE_URL_MALFORMAT: return -EINVAL;
    case CURLE_COULDNT_RESOLVE_HOST: return -EHOSTUNREACH;
    case CURLE_COULDNT_CONNECT: return -ECONNREFUSED;
    case CURLE_OPERATION_TIMEDOUT: return -ETIMEDOUT;
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_FTP_ACCEPT_FAILED:
    case CURLE_FTP_CANT_GET_HOST: return -ECONNRESET;
    case CURLE_LOGIN_DENIED: return -FR_EAUTH;
    case CURLE_REMOTE_FILE_NOT_FOUND: return -ENOENT;
    case CURLE_REMOTE_ACCESS_DENIED: return -EACCES;
    case CURLE_REMOTE_DISK_FULL: return -ENOSPC;
    case CURLE_REMOTE_FILE_EXISTS: return -EEXIST;
    case CURLE_FILESIZE_EXCEEDED: return -EFBIG;
    case CURLE_OUT_OF_MEMORY: return -ENOMEM;
    case CURLE_USE_SSL_FAILED: return -EPROTONOSUPPORT; /* the FTP server has no TLS */
    case CURLE_PEER_FAILED_VERIFICATION: return -EACCES;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_WEIRD_SERVER_REPLY:
    case CURLE_FTP_WEIRD_PASV_REPLY:
    case CURLE_FTP_WEIRD_227_FORMAT: return -EPROTO;
    default: return -EIO;
    }
}

/* res and the status as -errno, asking about the certificate or dropping refused credentials on the way */
static int result(fr_curl *h, CURLcode res, int http_any)
{
    curl_easy_getinfo(h->e, CURLINFO_RESPONSE_CODE, &h->status);
    if (res == CURLE_PEER_FAILED_VERIFICATION && h->seen) return untrusted(h);
    int http = h->proto == FR_CURL_DAV && (res == CURLE_OK || res == CURLE_HTTP_RETURNED_ERROR);
    if (res == CURLE_LOGIN_DENIED || (http && h->status == 401)) {
        if (h->had_creds) fr_creds_refuse(h->key);
        return -FR_EAUTH;
    }
    if (http) return res == CURLE_OK && (http_any || h->status < 300) ? 0 : fr_http_errno(h->status);
    if (res == CURLE_QUOTE_ERROR || res == CURLE_UPLOAD_FAILED || res == CURLE_FTP_COULDNT_RETR_FILE) {
        int e = fr_ftp_errno(h->status);
        if (e) return e;
    }
    return curl_errno(res);
}

static void apply(fr_curl *h)
{
    curl_easy_setopt(h->e, CURLOPT_HTTPHEADER, h->headers);
    curl_easy_setopt(h->e, CURLOPT_QUOTE, h->quote);
}

/* a server that wants another HTTP authentication than the Basic sent first: 1 when h now asks with it */
static int switch_auth(fr_curl *h)
{
    long avail = 0, want;
    if (h->proto != FR_CURL_DAV || h->status != 401 || !h->had_creds) return 0;
    curl_easy_getinfo(h->e, CURLINFO_HTTPAUTH_AVAIL, &avail);
    want = avail & (long)(CURLAUTH_BASIC | CURLAUTH_DIGEST);
    long now = __atomic_load_n(&h->srv->auth, __ATOMIC_RELAXED);
    if (!want || want == (now ? now : (long)CURLAUTH_BASIC)) return 0;
    __atomic_store_n(&h->srv->auth, want, __ATOMIC_RELAXED);
    curl_easy_setopt(h->e, CURLOPT_HTTPAUTH, want);
    return 1;
}

static CURLcode run(fr_curl *h)
{
    CURLcode res = CURLE_OK;
    if (curl_multi_add_handle(h->m, h->e)) return CURLE_OUT_OF_MEMORY;
    int running = 1;
    while (running) {
        if (curl_multi_perform(h->m, &running)) {
            res = CURLE_OUT_OF_MEMORY;
            break;
        }
        if (running) curl_multi_poll(h->m, NULL, 0, 1000, NULL);
    }
    CURLMsg *msg;
    int left;
    while ((msg = curl_multi_info_read(h->m, &left)))
        if (msg->msg == CURLMSG_DONE) res = msg->data.result;
    curl_multi_remove_handle(h->m, h->e);
    return res;
}

int fr_curl_perform(fr_curl *h, int http_any)
{
    apply(h);
    for (int attempt = 0;; attempt++) {
        h->body.n = h->reply.n = 0;
        h->seen = h->verify_error = h->pin_mismatch = 0;
        CURLcode res = run(h);
        long connects = 0;
        curl_easy_getinfo(h->e, CURLINFO_NUM_CONNECTS, &connects);
        curl_easy_getinfo(h->e, CURLINFO_RESPONSE_CODE, &h->status);
        if (attempt < 2 && switch_auth(h)) continue;
        int rc = result(h, res, http_any);
        /* a kept connection the server closed meanwhile: once more, on a new one */
        if (!attempt && !connects && fr_net_broken(rc)) continue;
        return rc;
    }
}

/* ---- streams ---- */

static size_t up_cb(char *buf, size_t size, size_t n, void *ud)
{
    fr_curl *h = ud;
    cstream *st = h->stream;
    st->started = 1;
    size_t want = size * n;
    if (st->up_n) {
        size_t k = st->up_n < want ? st->up_n : want;
        memcpy(buf, st->up, k);
        st->up += k, st->up_n -= k;
        return k;
    }
    if (st->eof) return 0;
    st->paused = 1;
    return CURL_READFUNC_PAUSE;
}

/* runs the transfer until want(st) or its end */
static void pump(cstream *st, int (*want)(const cstream *st))
{
    fr_curl *h = st->h;
    while (!st->done && !want(st)) {
        int running;
        if (curl_multi_perform(h->m, &running)) {
            st->done = 1, st->res = CURLE_OUT_OF_MEMORY;
            break;
        }
        CURLMsg *msg;
        int left;
        while ((msg = curl_multi_info_read(h->m, &left)))
            if (msg->msg == CURLMSG_DONE) st->done = 1, st->res = msg->data.result;
        if (!st->done && !want(st)) curl_multi_poll(h->m, NULL, 0, 1000, NULL);
    }
}

static int have_data(const cstream *st) { return st->h->body.n > st->off; }
static int started(const cstream *st) { return st->started; }
static int drained(const cstream *st) { return !st->up_n; }
static int never(const cstream *st) { return 0; }

static void resume(cstream *st)
{
    if (!st->paused) return;
    st->paused = 0;
    curl_easy_pause(st->h->e, CURLPAUSE_CONT);
}

static int stream_rc(cstream *st)
{
    int rc = result(st->h, st->res, 0);
    if (st->upload && st->finish) rc = st->finish(st->h, rc);
    return rc;
}

static void stream_free(cstream *st)
{
    fr_curl *h = st->h;
    curl_multi_remove_handle(h->m, h->e);
    h->stream = NULL;
    fr_curl_end(h);
    free(st);
}

static long st_read(fr_stream *p, void *buf, size_t n)
{
    cstream *st = (cstream *)p;
    fr_curl *h = st->h;
    pump(st, have_data);
    size_t avail = h->body.n - st->off;
    if (!avail) return st->res == CURLE_OK ? 0 : stream_rc(st);
    size_t k = avail < n ? avail : n;
    memcpy(buf, h->body.v + st->off, k);
    st->off += k;
    if (st->off == h->body.n) st->off = h->body.n = 0;
    if (h->body.n - st->off < READ_AHEAD / 2) resume(st);
    return (long)k;
}

static long st_write(fr_stream *p, const void *buf, size_t n)
{
    cstream *st = (cstream *)p;
    st->up = buf, st->up_n = n;
    resume(st);
    pump(st, drained);
    size_t left = st->up_n;
    st->up = NULL, st->up_n = 0;
    if (left) return st->res == CURLE_OK ? -EIO : result(st->h, st->res, 0);
    return (long)n;
}

static int st_close(fr_stream *p)
{
    cstream *st = (cstream *)p;
    int rc = 0;
    if (st->upload) {
        st->eof = 1;
        resume(st);
        pump(st, never);
        rc = stream_rc(st);
    }
    stream_free(st);
    return rc;
}

fr_stream *fr_curl_stream(fr_curl *h, int upload, int (*finish)(fr_curl *h, int rc), int *err)
{
    apply(h);
    cstream *st = fr_realloc(NULL, sizeof *st);
    *st = (cstream){{st_read, st_write, st_close}, h, upload, .finish = finish};
    h->stream = st;
    h->body.n = h->reply.n = 0;
    h->seen = h->verify_error = h->pin_mismatch = 0;
    if (upload) {
        curl_easy_setopt(h->e, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(h->e, CURLOPT_READFUNCTION, up_cb);
        curl_easy_setopt(h->e, CURLOPT_READDATA, h);
    }
    if (curl_multi_add_handle(h->m, h->e)) {
        *err = -ENOMEM;
        h->stream = NULL;
        fr_curl_end(h);
        free(st);
        return NULL;
    }
    /* the server's yes or no before the caller goes on: the first data, or the upload asked for */
    pump(st, upload ? started : have_data);
    if (st->done && (*err = result(h, st->res, 0))) {
        stream_free(st);
        return NULL;
    }
    return &st->s;
}

/* ---- a plain HTTPS GET, outside the servers' pool ---- */

static size_t file_cb(char *p, size_t size, size_t n, void *ud) { return fwrite(p, size, n, ud); }

static size_t buf_cb(char *p, size_t size, size_t n, void *ud)
{
    fr_curl_buf_add(ud, p, size * n);
    return size * n;
}

int fr_curl_fetch(const char *url, fr_curl_buf *body, const char *path)
{
    pthread_once(&once, global_init);
    CURL *e = curl_easy_init();
    if (!e) return -ENOMEM;
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (path && !f) {
        int rc = -errno;
        curl_easy_cleanup(e);
        return rc;
    }
    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(e, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT_MS, (long)CONNECT_TIMEOUT_MS);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, (long)STALL_S);
    curl_easy_setopt(e, CURLOPT_USERAGENT, "ferret/" FR_VERSION);
    if (!path) curl_easy_setopt(e, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, f ? file_cb : buf_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, f ? (void *)f : (void *)body);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
    if (ca_blob.n) {
        struct curl_blob blob = {ca_blob.v, ca_blob.n, CURL_BLOB_NOCOPY};
        curl_easy_setopt(e, CURLOPT_CAINFO_BLOB, &blob);
    } else if (ca_file) curl_easy_setopt(e, CURLOPT_CAINFO, ca_file);
    CURLcode r = curl_easy_perform(e);
    long status = 0;
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(e);
    int rc = r != CURLE_OK ? -EIO : status >= 300 ? fr_http_errno(status) : 0;
    if (f && fclose(f) && !rc) rc = -errno;
    if (f && rc) unlink(path);
    return rc;
}

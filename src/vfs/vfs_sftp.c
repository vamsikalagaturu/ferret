/* sftp://[user@]host[:port]/path over libssh2. The server's host key is checked against ~/.ssh/known_hosts (read
 * only) and ferret's own known_hosts, where a key the user accepted on first use is added (OpenSSH's format). Sign-in
 * tries ssh-agent and the keys in ~/.ssh, then the password given for the server. */
#define _GNU_SOURCE
#include "net/net.h"
#include "vfs.h"
#include <errno.h>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONNECT_TIMEOUT_MS 10000
#define SESSION_TIMEOUT_MS 30000

typedef struct {
    int sock;
    LIBSSH2_SESSION *ssh;
    LIBSSH2_SFTP *sftp;
} session;

/* ---- errors ---- */

static int sftp_errno(unsigned long fx)
{
    switch (fx) {
    case LIBSSH2_FX_NO_SUCH_FILE:
    case LIBSSH2_FX_NO_SUCH_PATH: return -ENOENT;
    case LIBSSH2_FX_PERMISSION_DENIED:
    case LIBSSH2_FX_UNKNOWN_PRINCIPAL: return -EACCES;
    case LIBSSH2_FX_WRITE_PROTECT: return -EROFS;
    case LIBSSH2_FX_NO_CONNECTION:
    case LIBSSH2_FX_CONNECTION_LOST: return -ECONNRESET;
    case LIBSSH2_FX_OP_UNSUPPORTED: return -ENOTSUP;
    case LIBSSH2_FX_INVALID_HANDLE: return -EBADF;
    case LIBSSH2_FX_FILE_ALREADY_EXISTS: return -EEXIST;
    case LIBSSH2_FX_NO_MEDIA: return -ENODEV;
    case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM: return -ENOSPC;
    case LIBSSH2_FX_QUOTA_EXCEEDED: return -EDQUOT;
    case LIBSSH2_FX_LOCK_CONFLICT: return -EBUSY;
    case LIBSSH2_FX_DIR_NOT_EMPTY: return -ENOTEMPTY;
    case LIBSSH2_FX_NOT_A_DIRECTORY: return -ENOTDIR;
    case LIBSSH2_FX_INVALID_FILENAME: return -EINVAL;
    case LIBSSH2_FX_LINK_LOOP: return -ELOOP;
    default: return -EIO;
    }
}

/* the last call's failure on s as -errno */
static int last_error(const session *s)
{
    switch (libssh2_session_last_errno(s->ssh)) {
    case LIBSSH2_ERROR_SFTP_PROTOCOL: return sftp_errno(libssh2_sftp_last_error(s->sftp));
    case LIBSSH2_ERROR_TIMEOUT:
    case LIBSSH2_ERROR_SOCKET_TIMEOUT: return -ETIMEDOUT;
    case LIBSSH2_ERROR_SOCKET_NONE:
    case LIBSSH2_ERROR_SOCKET_SEND:
    case LIBSSH2_ERROR_SOCKET_RECV:
    case LIBSSH2_ERROR_SOCKET_DISCONNECT:
    case LIBSSH2_ERROR_CHANNEL_CLOSED: return -ECONNRESET;
    case LIBSSH2_ERROR_ALLOC: return -ENOMEM;
    default: return -EIO;
    }
}

/* ---- host keys ---- */

static char *config_known_hosts(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg == '/') return fr_format("%s/ferret/known_hosts", xdg);
    return home && *home ? fr_format("%s/.config/ferret/known_hosts", home) : NULL;
}

static char *base64(const unsigned char *p, size_t n, int pad)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = fr_realloc(NULL, (n + 2) / 3 * 4 + 1), *o = out;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        *o++ = abc[v >> 18 & 63];
        *o++ = abc[v >> 12 & 63];
        if (i + 1 < n || pad) *o++ = i + 1 < n ? abc[v >> 6 & 63] : '=';
        if (i + 2 < n || pad) *o++ = i + 2 < n ? abc[v & 63] : '=';
    }
    *o = 0;
    return out;
}

static const struct {
    int hostkey, knownhost;
    const char *name;
} key_types[] = {
    {LIBSSH2_HOSTKEY_TYPE_ED25519, LIBSSH2_KNOWNHOST_KEY_ED25519, "ssh-ed25519"},
    {LIBSSH2_HOSTKEY_TYPE_ECDSA_256, LIBSSH2_KNOWNHOST_KEY_ECDSA_256, "ecdsa-sha2-nistp256"},
    {LIBSSH2_HOSTKEY_TYPE_ECDSA_384, LIBSSH2_KNOWNHOST_KEY_ECDSA_384, "ecdsa-sha2-nistp384"},
    {LIBSSH2_HOSTKEY_TYPE_ECDSA_521, LIBSSH2_KNOWNHOST_KEY_ECDSA_521, "ecdsa-sha2-nistp521"},
    {LIBSSH2_HOSTKEY_TYPE_RSA, LIBSSH2_KNOWNHOST_KEY_SSHRSA, "ssh-rsa"},
};

/* LIBSSH2_KNOWNHOST_CHECK_* of the key against one file; libssh2_knownhost_readfile() stops at the first line it
 * cannot parse (a key type it lacks, a marker), so the lines go in one by one */
static int check_file(LIBSSH2_SESSION *ssh, const char *path, const char *host, int port, const char *key, size_t len,
                      int type)
{
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return LIBSSH2_KNOWNHOST_CHECK_NOTFOUND;
    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(ssh);
    char line[8192];
    while (kh && fgets(line, sizeof line, f)) libssh2_knownhost_readline(kh, line, strlen(line), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    fclose(f);
    if (!kh) return LIBSSH2_KNOWNHOST_CHECK_FAILURE;
    int r = libssh2_knownhost_checkp(kh, host, port, key, len, LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | type, NULL);
    libssh2_knownhost_free(kh);
    return r;
}

static void accept_key(void *line)
{
    char *path = config_known_hosts();
    if (!path) return;
    char *dir = fr_strndup(path, (size_t)(strrchr(path, '/') - path));
    fr_mkdirs(dir);
    FILE *f = fopen(path, "a");
    if (f) {
        fputs(line, f);
        if (fclose(f)) fprintf(stderr, "ferret: could not write %s\n", path);
    }
    free(dir);
    free(path);
}

static int check_host_key(const session *s, const char *key, const fr_net_uri *u)
{
    size_t len;
    int type;
    const char *blob = libssh2_session_hostkey(s->ssh, &len, &type);
    int k = -1;
    for (int i = 0; blob && i < (int)(sizeof key_types / sizeof *key_types); i++)
        if (key_types[i].hostkey == type) k = i;
    if (k < 0) return -ENOTSUP;
    const char *home = getenv("HOME");
    char *user_file = home ? fr_format("%s/.ssh/known_hosts", home) : NULL, *own = config_known_hosts();
    int a = check_file(s->ssh, user_file, u->host, u->port, blob, len, key_types[k].knownhost);
    int b = check_file(s->ssh, own, u->host, u->port, blob, len, key_types[k].knownhost);
    free(user_file), free(own);
    if (a == LIBSSH2_KNOWNHOST_CHECK_MATCH || b == LIBSSH2_KNOWNHOST_CHECK_MATCH) return 0;
    if (a == LIBSSH2_KNOWNHOST_CHECK_MISMATCH || b == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) return -FR_EIDCHANGED;
    const char *sha = libssh2_hostkey_hash(s->ssh, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!sha) return -EIO;
    char *fingerprint = base64((const unsigned char *)sha, 32, 0), *encoded = base64((const unsigned char *)blob, len, 1);
    int v6 = strchr(u->host, ':') != NULL;
    char *name = u->port == 22 && !v6 ? fr_strdup(u->host) : fr_format("[%s]:%d", u->host, u->port);
    char *line = fr_format("%s %s %s\n", name, key_types[k].name, encoded);
    char *question = fr_format("ferret has not met %s before. Its %s key's fingerprint is SHA256:%s. Compare it with "
                               "the server's own (ssh-keygen -lf on its host key) before you trust it.",
                               name, key_types[k].name, fingerprint);
    fr_trust_ask(key, question, accept_key, line, free);
    free(fingerprint), free(encoded), free(name), free(question);
    return -FR_EUNTRUSTED;
}

/* ---- signing in ---- */

static _Thread_local const char *kbd_password;

static void kbd_answer(const char *name, int name_len, const char *instruction, int instruction_len, int num_prompts,
                       const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts, LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                       void **abstract)
{
    for (int i = 0; i < num_prompts; i++) {
        responses[i].text = fr_strdup(kbd_password ? kbd_password : "");
        responses[i].length = (unsigned)strlen(responses[i].text);
    }
}

static int try_keys(LIBSSH2_SESSION *ssh, const char *user)
{
#ifndef __ANDROID__
    if (getenv("SSH_AUTH_SOCK")) {
        LIBSSH2_AGENT *agent = libssh2_agent_init(ssh);
        int ok = 0;
        if (agent && !libssh2_agent_connect(agent) && !libssh2_agent_list_identities(agent)) {
            struct libssh2_agent_publickey *id = NULL, *prev = NULL;
            while (!ok && !libssh2_agent_get_identity(agent, &id, prev)) {
                ok = !libssh2_agent_userauth(agent, user, id);
                prev = id;
            }
            libssh2_agent_disconnect(agent);
        }
        if (agent) libssh2_agent_free(agent);
        if (ok) return 1;
    }
#endif
    const char *home = getenv("HOME");
    static const char *const names[] = {"id_ed25519", "id_ecdsa", "id_rsa"};
    for (size_t i = 0; home && i < sizeof names / sizeof *names; i++) {
        char *priv = fr_format("%s/.ssh/%s", home, names[i]);
        int ok = !access(priv, R_OK) &&
                 !libssh2_userauth_publickey_fromfile_ex(ssh, user, (unsigned)strlen(user), NULL, priv, NULL);
        free(priv);
        if (ok) return 1;
        if (libssh2_session_last_errno(ssh) == LIBSSH2_ERROR_SOCKET_DISCONNECT) return 0;
    }
    return 0;
}

/* 0 signed in; keys: try the agent and ~/.ssh first */
static int sign_in(session *s, const char *key, const fr_net_uri *u, int keys)
{
    fr_cred cred = {0};
    int have = !fr_creds_get(key, &cred);
    const char *user = have && *cred.user ? cred.user : u->user ? u->user : getenv("USER");
    int rc = -FR_EAUTH;
    if (!user || !*user) goto done;
    char *methods = libssh2_userauth_list(s->ssh, user, (unsigned)strlen(user));
    if (!methods) {
        rc = libssh2_userauth_authenticated(s->ssh) ? 0 : last_error(s);
        goto done;
    }
    if (keys && strstr(methods, "publickey") && try_keys(s->ssh, user)) {
        rc = 0;
        goto done;
    }
    if (!have) goto done;
    int r = LIBSSH2_ERROR_AUTHENTICATION_FAILED;
    if (strstr(methods, "password")) r = libssh2_userauth_password(s->ssh, user, cred.password);
    if (r == LIBSSH2_ERROR_AUTHENTICATION_FAILED && strstr(methods, "keyboard-interactive")) {
        kbd_password = cred.password;
        r = libssh2_userauth_keyboard_interactive(s->ssh, user, kbd_answer);
        kbd_password = NULL;
    }
    if (!r) rc = 0;
    else if (r == LIBSSH2_ERROR_AUTHENTICATION_FAILED || r == LIBSSH2_ERROR_PASSWORD_EXPIRED) fr_creds_refuse(key);
    else rc = last_error(s);
done:
    fr_cred_free(&cred);
    return rc;
}

/* ---- sessions ---- */

static void session_close(void *p)
{
    session *s = p;
    if (s->sftp) libssh2_sftp_shutdown(s->sftp);
    if (s->ssh) {
        libssh2_session_disconnect(s->ssh, "bye");
        libssh2_session_free(s->ssh);
    }
    if (s->sock >= 0) close(s->sock);
    free(s);
}

static void init_libssh2(void) { libssh2_init(0); }

static session *session_open(const char *key, const fr_net_uri *u, int keys, int *err)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_libssh2);
    session *s = fr_realloc(NULL, sizeof *s);
    *s = (session){.sock = fr_net_connect(u->host, u->port, CONNECT_TIMEOUT_MS, err)};
    if (s->sock < 0) goto fail;
    if (!(s->ssh = libssh2_session_init())) {
        *err = -ENOMEM;
        goto fail;
    }
    libssh2_session_set_blocking(s->ssh, 1);
    libssh2_session_set_timeout(s->ssh, SESSION_TIMEOUT_MS);
    if (libssh2_session_handshake(s->ssh, s->sock)) {
        *err = libssh2_session_last_errno(s->ssh) == LIBSSH2_ERROR_KEX_FAILURE ? -EPROTO : last_error(s);
        goto fail;
    }
    if ((*err = check_host_key(s, key, u)) || (*err = sign_in(s, key, u, keys))) goto fail;
    if (!(s->sftp = libssh2_sftp_init(s->ssh))) {
        *err = last_error(s) == -EIO ? -EPROTONOSUPPORT : last_error(s);
        goto fail;
    }
    return s;
fail:
    session_close(s);
    return NULL;
}

static void *sftp_connect(const char *key, const fr_net_uri *u, int *err)
{
    session *s = session_open(key, u, 1, err);
    fr_cred cred = {0};
    /* servers cut a sign-in off after a few refused keys (OpenSSH's MaxAuthTries), before the password got its turn */
    if (!s && fr_net_broken(*err) && !fr_creds_get(key, &cred)) s = session_open(key, u, 0, err);
    fr_cred_free(&cred);
    return s;
}

static const fr_conn_ops ops = {sftp_connect, session_close};

/* ---- calls ---- */

typedef int (*call_fn)(session *s, const char *path, void *ctx);

/* fn on uri's server, retried once on a new connection when a reused one turned out to be gone */
static int call(const char *uri, call_fn fn, void *ctx)
{
    fr_net_uri u;
    if (fr_net_parse(uri, &u)) return -EINVAL;
    char *key = fr_net_key(&u);
    int rc;
    for (int attempt = 0;; attempt++) {
        int fresh;
        fr_conn *c = fr_conn_get(key, &u, &ops, &fresh, &rc);
        if (!c) break;
        rc = fn(fr_conn_session(c), u.path, ctx);
        int broken = fr_net_broken(rc);
        fr_conn_put(c, broken);
        if (!broken || fresh || attempt) break;
    }
    free(key);
    fr_net_uri_free(&u);
    return rc;
}

static void fill(fr_entry *e, const char *name, const LIBSSH2_SFTP_ATTRIBUTES *a)
{
    e->name = fr_strdup(name);
    e->hidden = name[0] == '.';
    e->is_link = (a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISLNK(a->permissions);
    e->is_dir = (a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a->permissions);
    e->size = e->is_dir || !(a->flags & LIBSSH2_SFTP_ATTR_SIZE) ? 0 : a->filesize;
    e->mtime = a->flags & LIBSSH2_SFTP_ATTR_ACMODTIME ? (int64_t)a->mtime : 0;
    e->mode = a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS ? (uint32_t)(a->permissions & 07777) : 0;
}

/* a link's target decides whether it shows as a folder, and its size */
static void follow(session *s, const char *path, fr_entry *e)
{
    LIBSSH2_SFTP_ATTRIBUTES t;
    if (libssh2_sftp_stat_ex(s->sftp, path, (unsigned)strlen(path), LIBSSH2_SFTP_STAT, &t)) return;
    e->is_dir = (t.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(t.permissions);
    if (!e->is_dir && (t.flags & LIBSSH2_SFTP_ATTR_SIZE)) e->size = t.filesize;
}

static int do_list(session *s, const char *path, void *ctx)
{
    fr_entries *out = ctx, got = {0};
    LIBSSH2_SFTP_HANDLE *d = libssh2_sftp_open_ex(s->sftp, path, (unsigned)strlen(path), 0, 0, LIBSSH2_SFTP_OPENDIR);
    if (!d) return last_error(s);
    char name[4096];
    LIBSSH2_SFTP_ATTRIBUTES a;
    int n, rc = 0;
    while ((n = libssh2_sftp_readdir(d, name, sizeof name, &a)) > 0) {
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        fr_entry e = {0};
        fill(&e, name, &a);
        if (e.is_link) {
            char *child = fr_format("%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", name);
            follow(s, child, &e);
            free(child);
        }
        FR_PUSH(got, e);
    }
    if (n < 0) rc = last_error(s);
    libssh2_sftp_closedir(d);
    if (rc) fr_entries_free(&got);
    else *out = got;
    return rc;
}

static int sftp_list(const char *uri, fr_entries *out) { return call(uri, do_list, out); }

static int do_stat(session *s, const char *path, void *ctx)
{
    LIBSSH2_SFTP_ATTRIBUTES a;
    if (libssh2_sftp_stat_ex(s->sftp, path, (unsigned)strlen(path), LIBSSH2_SFTP_LSTAT, &a)) return last_error(s);
    const char *slash = strrchr(path, '/'), *name = slash && slash[1] ? slash + 1 : "/";
    fr_entry *e = ctx;
    fill(e, name, &a);
    if (e->is_link) follow(s, path, e);
    return 0;
}

static int sftp_stat(const char *uri, fr_entry *out) { return call(uri, do_stat, out); }

/* OpenSSH answers "failure" where SFTP version 3 has no better word, as for a name that is taken */
static int taken_or(session *s, const char *path, int rc)
{
    LIBSSH2_SFTP_ATTRIBUTES a;
    if (rc == -EIO && !libssh2_sftp_stat_ex(s->sftp, path, (unsigned)strlen(path), LIBSSH2_SFTP_LSTAT, &a)) return -EEXIST;
    return rc;
}

static int do_mkdir(session *s, const char *path, void *ctx)
{
    if (!libssh2_sftp_mkdir_ex(s->sftp, path, (unsigned)strlen(path), 0777)) return 0;
    return taken_or(s, path, last_error(s));
}

static int sftp_mkdir(const char *uri) { return call(uri, do_mkdir, NULL); }

static int do_remove(session *s, const char *path, void *ctx)
{
    LIBSSH2_SFTP_ATTRIBUTES a;
    unsigned n = (unsigned)strlen(path);
    if (libssh2_sftp_stat_ex(s->sftp, path, n, LIBSSH2_SFTP_LSTAT, &a)) return last_error(s);
    int dir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a.permissions);
    int r = dir ? libssh2_sftp_rmdir_ex(s->sftp, path, n) : libssh2_sftp_unlink_ex(s->sftp, path, n);
    return r ? last_error(s) : 0;
}

static int sftp_remove(const char *uri) { return call(uri, do_remove, NULL); }

static int do_rename(session *s, const char *path, void *ctx)
{
    const char *to = ctx;
    LIBSSH2_SFTP_ATTRIBUTES a;
    /* SFTP v3 leaves replacing to the server; callers decide about conflicts first, as rename(2) would not */
    if (!libssh2_sftp_stat_ex(s->sftp, to, (unsigned)strlen(to), LIBSSH2_SFTP_LSTAT, &a)) return -EEXIST;
    if (libssh2_sftp_rename_ex(s->sftp, path, (unsigned)strlen(path), to, (unsigned)strlen(to),
                               LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE))
        return last_error(s);
    return 0;
}

static int sftp_rename(const char *from, const char *to)
{
    if (!fr_uri_same_backend(from, to)) return -EXDEV;
    return call(from, do_rename, (void *)fr_uri_path(to));
}

typedef struct {
    int64_t mtime;
    uint32_t mode;
    int what; /* 0 mtime, 1 mode */
} setattr;

static int do_setattr(session *s, const char *path, void *ctx)
{
    setattr *w = ctx;
    LIBSSH2_SFTP_ATTRIBUTES a;
    unsigned n = (unsigned)strlen(path);
    if (libssh2_sftp_stat_ex(s->sftp, path, n, LIBSSH2_SFTP_STAT, &a)) return last_error(s);
    LIBSSH2_SFTP_ATTRIBUTES set = {0};
    if (w->what == 0) {
        set.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
        set.atime = a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME ? a.atime : (unsigned long)w->mtime;
        set.mtime = (unsigned long)w->mtime;
    } else {
        set.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
        set.permissions = (a.permissions & ~07777UL) | w->mode;
    }
    return libssh2_sftp_stat_ex(s->sftp, path, n, LIBSSH2_SFTP_SETSTAT, &set) ? last_error(s) : 0;
}

static int sftp_set_mtime(const char *uri, int64_t mtime) { return call(uri, do_setattr, &(setattr){mtime, 0, 0}); }

static int sftp_set_mode(const char *uri, uint32_t mode) { return call(uri, do_setattr, &(setattr){0, mode, 1}); }

static int do_free_space(session *s, const char *path, void *ctx)
{
    LIBSSH2_SFTP_STATVFS st;
    if (libssh2_sftp_statvfs(s->sftp, path, strlen(path), &st)) return last_error(s) == -EIO ? -ENOTSUP : last_error(s);
    uint64_t *v = ctx;
    v[0] = (uint64_t)st.f_bavail * st.f_frsize;
    v[1] = (uint64_t)st.f_blocks * st.f_frsize;
    return 0;
}

static int sftp_free_space(const char *uri, uint64_t *free_bytes, uint64_t *total_bytes)
{
    uint64_t v[2];
    int rc = call(uri, do_free_space, v);
    if (!rc) *free_bytes = v[0], *total_bytes = v[1];
    return rc;
}

/* ---- streams: they keep the connection between calls and lock it for each one ---- */

typedef struct {
    fr_stream s;
    fr_conn *c;
    unsigned generation;
    LIBSSH2_SFTP_HANDLE *h;
} file_stream;

static long st_read(fr_stream *p, void *buf, size_t n)
{
    file_stream *f = (file_stream *)p;
    int rc = fr_conn_relock(f->c, f->generation);
    if (rc) return rc;
    session *s = fr_conn_session(f->c);
    ssize_t r = libssh2_sftp_read(f->h, buf, n);
    long out = r >= 0 ? (long)r : last_error(s);
    fr_conn_unlock(f->c, r < 0 && fr_net_broken((int)out));
    return out;
}

static long st_write(fr_stream *p, const void *buf, size_t n)
{
    file_stream *f = (file_stream *)p;
    int rc = fr_conn_relock(f->c, f->generation);
    if (rc) return rc;
    session *s = fr_conn_session(f->c);
    size_t done = 0;
    long out = 0;
    while (done < n) {
        ssize_t r = libssh2_sftp_write(f->h, (const char *)buf + done, n - done);
        if (r < 0) {
            out = last_error(s);
            break;
        }
        done += (size_t)r;
    }
    fr_conn_unlock(f->c, out < 0 && fr_net_broken((int)out));
    return out < 0 ? out : (long)done;
}

static int st_close(fr_stream *p)
{
    file_stream *f = (file_stream *)p;
    int rc = fr_conn_relock(f->c, f->generation);
    if (!rc) {
        if (libssh2_sftp_close_handle(f->h)) rc = last_error(fr_conn_session(f->c));
        fr_conn_unlock(f->c, fr_net_broken(rc));
    }
    fr_conn_unref(f->c);
    free(f);
    return rc;
}

typedef struct {
    unsigned long flags;
    LIBSSH2_SFTP_HANDLE *h;
} opening;

static int do_open(session *s, const char *path, void *ctx)
{
    opening *o = ctx;
    o->h = libssh2_sftp_open_ex(s->sftp, path, (unsigned)strlen(path), o->flags, 0666, LIBSSH2_SFTP_OPENFILE);
    if (o->h) return 0;
    int rc = last_error(s);
    return o->flags & LIBSSH2_FXF_EXCL ? taken_or(s, path, rc) : rc;
}

static fr_stream *open_stream(const char *uri, unsigned long flags, int *err)
{
    fr_net_uri u;
    if (fr_net_parse(uri, &u)) return *err = -EINVAL, NULL;
    char *key = fr_net_key(&u);
    file_stream *f = NULL;
    for (int attempt = 0;; attempt++) {
        int fresh;
        fr_conn *c = fr_conn_get(key, &u, &ops, &fresh, err);
        if (!c) break;
        opening o = {flags, NULL};
        *err = do_open(fr_conn_session(c), u.path, &o);
        int broken = fr_net_broken(*err);
        if (o.h) {
            f = fr_realloc(NULL, sizeof *f);
            *f = (file_stream){{st_read, st_write, st_close}, c, fr_conn_generation(c), o.h};
            fr_conn_unlock(c, 0);
            break;
        }
        fr_conn_put(c, broken);
        if (!broken || fresh || attempt) break;
    }
    free(key);
    fr_net_uri_free(&u);
    return f ? &f->s : NULL;
}

static fr_stream *sftp_open_read(const char *uri, int *err) { return open_stream(uri, LIBSSH2_FXF_READ, err); }

static fr_stream *sftp_open_write(const char *uri, int exclusive, int *err)
{
    return open_stream(uri, LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | (exclusive ? LIBSSH2_FXF_EXCL : LIBSSH2_FXF_TRUNC), err);
}

const fr_backend fr_backend_sftp = {
    .scheme = "sftp",
    .list = sftp_list,
    .stat = sftp_stat,
    .open_read = sftp_open_read,
    .open_write = sftp_open_write,
    .mkdir = sftp_mkdir,
    .rename = sftp_rename,
    .remove = sftp_remove,
    .set_mtime = sftp_set_mtime,
    .free_space = sftp_free_space,
    .set_mode = sftp_set_mode,
};

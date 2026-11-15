/* smb://[[domain;]user@]host[:port]/share/path over libsmb2 (SMB 2 and 3, NTLM sign-in). libsmb2 connects a context
 * to one share, so the pool keeps a connection per share; smb://host/ lists the server's shares through IPC$. Without
 * a password given for the server it signs in as "guest", which a server either takes or refuses (then the window
 * asks for one). */
#define _GNU_SOURCE
#include "net/net.h"
#include "vfs.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

#define TIMEOUT_S 30

typedef struct {
    struct smb2_context *ctx;
    uint32_t max_read, max_write;
} session;

/* the share a path is in, and the path within it ("" at the share's root) */
typedef struct {
    char *share;
    const char *rest;
} place;

static place place_of(const char *path)
{
    const char *seg = path + 1, *end = strchr(seg, '/');
    place p = {fr_strndup(seg, end ? (size_t)(end - seg) : strlen(seg)), end ? end + 1 : ""};
    return p;
}

static int sign_in_refused(uint32_t nt)
{
    switch (nt) {
    case SMB2_STATUS_LOGON_FAILURE:
    case SMB2_STATUS_WRONG_PASSWORD:
    case SMB2_STATUS_WRONG_PASSWORD_CORE:
    case SMB2_STATUS_NO_SUCH_USER:
    case SMB2_STATUS_PASSWORD_EXPIRED:
    case SMB2_STATUS_ACCOUNT_RESTRICTION: return 1;
    default: return 0;
    }
}

static void session_close(void *p)
{
    session *s = p;
    smb2_disconnect_share(s->ctx);
    smb2_destroy_context(s->ctx);
    free(s);
}

static void *smb_connect(const char *key, const fr_net_uri *u, int *err)
{
    place p = place_of(u->path);
    const char *share = *p.share ? p.share : "IPC$";
    char *cred_key = fr_net_key(u);
    fr_cred cred = {0};
    int have = !fr_creds_get(cred_key, &cred);
    char *user = fr_strdup(have && *cred.user ? cred.user : u->user ? u->user : "guest"), *domain = NULL;
    char *sep = strpbrk(user, ";\\");
    if (sep) {
        domain = user;
        *sep = 0;
        user = fr_strdup(sep + 1);
    }
    session *s = fr_realloc(NULL, sizeof *s);
    *s = (session){.ctx = smb2_init_context()};
    int rc = s->ctx ? 0 : -ENOMEM;
    if (!rc) {
        smb2_set_security_mode(s->ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
        smb2_set_timeout(s->ctx, TIMEOUT_S);
        smb2_set_user(s->ctx, user);
        smb2_set_password(s->ctx, have ? cred.password : "");
        if (domain) smb2_set_domain(s->ctx, domain);
        int v6 = strchr(u->host, ':') != NULL;
        char *server = fr_format("%s%s%s:%d", v6 ? "[" : "", u->host, v6 ? "]" : "", u->port);
        rc = smb2_connect_share(s->ctx, server, share, user);
        free(server);
        if (rc) {
            uint32_t nt = (uint32_t)smb2_get_nterror(s->ctx);
            if (sign_in_refused(nt) || (!have && nt == SMB2_STATUS_ACCESS_DENIED)) {
                if (have) fr_creds_refuse(cred_key);
                rc = -FR_EAUTH;
            } else if (nt == SMB2_STATUS_BAD_NETWORK_NAME) rc = -ENOENT;
            else if (rc == -EINVAL) rc = -EHOSTUNREACH; /* the name did not resolve */
        }
    }
    if (!rc) {
        s->max_read = smb2_get_max_read_size(s->ctx);
        s->max_write = smb2_get_max_write_size(s->ctx);
    }
    free(user), free(domain), free(p.share), free(cred_key);
    fr_cred_free(&cred);
    if (rc) {
        if (s->ctx) smb2_destroy_context(s->ctx);
        free(s);
        *err = rc;
        return NULL;
    }
    return s;
}

static const fr_conn_ops ops = {smb_connect, session_close};

/* the server closed the socket, or it failed: libsmb2 keeps the descriptor and says only that its call failed */
static int dead(session *s)
{
    int fd = smb2_get_fd(s->ctx);
    if (fd < 0) return 1;
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 0) <= 0) return 0;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return 1;
    char c;
    return (p.revents & POLLIN) && recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

/* rc from a libsmb2 call on s, or -ECONNRESET when the socket went with it */
static int checked(session *s, int rc)
{
    if (rc < 0 && !fr_net_broken(rc) && dead(s)) return -ECONNRESET;
    return rc;
}

/* the failure of a call that returned NULL, from the server's status */
static int nt_error(session *s)
{
    int e = nterror_to_errno((uint32_t)smb2_get_nterror(s->ctx));
    return checked(s, e > 0 ? -e : -EIO);
}

typedef int (*call_fn)(session *s, const char *share, const char *rest, void *ctx);

/* fn on the share of uri, retried once on a new connection when a reused one turned out to be gone */
static int call(const char *uri, call_fn fn, void *ctx)
{
    fr_net_uri u;
    if (fr_net_parse(uri, &u)) return -EINVAL;
    char *key = fr_net_key_share(&u);
    place p = place_of(u.path);
    int rc;
    for (int attempt = 0;; attempt++) {
        int fresh;
        fr_conn *c = fr_conn_get(key, &u, &ops, &fresh, &rc);
        if (!c) break;
        session *s = fr_conn_session(c);
        rc = checked(s, fn(s, p.share, p.rest, ctx));
        int broken = fr_net_broken(rc);
        fr_conn_put(c, broken);
        if (!broken || fresh || attempt) break;
    }
    free(p.share);
    free(key);
    fr_net_uri_free(&u);
    return rc;
}

static void fill(fr_entry *e, const char *name, const struct smb2_stat_64 *st)
{
    e->name = fr_strdup(name);
    e->hidden = name[0] == '.';
    e->is_link = st->smb2_type == SMB2_TYPE_LINK;
    e->is_dir = st->smb2_type == SMB2_TYPE_DIRECTORY;
    e->size = e->is_dir ? 0 : st->smb2_size;
    e->mtime = (int64_t)st->smb2_mtime;
}

/* ---- the server's shares ---- */

typedef struct {
    int done, rc;
    fr_entries *out;
} enumeration;

static void shares_cb(struct smb2_context *ctx, int status, void *data, void *priv)
{
    enumeration *en = priv;
    en->done = 1;
    if (status) {
        en->rc = status < 0 ? status : -EIO;
        return;
    }
    struct srvsvc_NetrShareEnum_rep *rep = data;
    struct srvsvc_SHARE_INFO_1_CONTAINER *c = &rep->ses.ShareInfo.Level1;
    for (uint32_t i = 0; c->Buffer && i < c->EntriesRead; i++) {
        struct srvsvc_SHARE_INFO_1 *si = &c->Buffer->share_info_1[i];
        if ((si->type & 3) != SHARE_TYPE_DISKTREE || !si->netname.utf8) continue;
        fr_entry e = {.name = fr_strdup(si->netname.utf8), .is_dir = 1};
        size_t n = strlen(e.name);
        e.hidden = (si->type & SHARE_TYPE_HIDDEN) || (n && e.name[n - 1] == '$');
        FR_PUSH(*en->out, e);
    }
    smb2_free_data(ctx, rep);
}

static int list_shares(session *s, fr_entries *out)
{
    enumeration en = {0, 0, out};
    int rc = smb2_share_enum_async(s->ctx, SHARE_INFO_1, shares_cb, &en);
    if (rc) return rc;
    time_t until = time(NULL) + TIMEOUT_S;
    while (!en.done) {
        struct pollfd p = {smb2_get_fd(s->ctx), (short)smb2_which_events(s->ctx), 0};
        if (p.fd < 0) return -ECONNRESET;
        int r = poll(&p, 1, 1000);
        if (r < 0 && errno != EINTR) return -errno;
        if (time(NULL) > until) return -ETIMEDOUT;
        if (r > 0 && smb2_service(s->ctx, p.revents) < 0) return -ECONNRESET;
    }
    return en.rc;
}

/* ---- calls ---- */

static int do_list(session *s, const char *share, const char *rest, void *ctx)
{
    fr_entries *out = ctx, got = {0};
    int rc = 0;
    if (!*share) rc = list_shares(s, &got);
    else {
        struct smb2dir *d = smb2_opendir(s->ctx, rest);
        if (!d) return nt_error(s);
        struct smb2dirent *de;
        while ((de = smb2_readdir(s->ctx, d))) {
            if (!strcmp(de->name, ".") || !strcmp(de->name, "..")) continue;
            fr_entry e = {0};
            fill(&e, de->name, &de->st);
            FR_PUSH(got, e);
        }
        smb2_closedir(s->ctx, d);
    }
    if (rc) fr_entries_free(&got);
    else *out = got;
    return rc;
}

static int smb_list(const char *uri, fr_entries *out) { return call(uri, do_list, out); }

static int do_stat(session *s, const char *share, const char *rest, void *ctx)
{
    fr_entry *e = ctx;
    const char *slash = strrchr(rest, '/'), *name = slash ? slash + 1 : *rest ? rest : *share ? share : "/";
    struct smb2_stat_64 st = {.smb2_type = SMB2_TYPE_DIRECTORY};
    /* the server and a share's root are folders; connecting proved they are there */
    if (*rest) {
        int rc = smb2_stat(s->ctx, rest, &st);
        if (rc) return rc;
    }
    fill(e, name, &st);
    return 0;
}

static int smb_stat(const char *uri, fr_entry *out) { return call(uri, do_stat, out); }

static int in_share(const char *share, const char *rest) { return *share && *rest ? 0 : -EPERM; }

static int do_mkdir(session *s, const char *share, const char *rest, void *ctx)
{
    int rc = in_share(share, rest);
    return rc ? rc : smb2_mkdir(s->ctx, rest);
}

static int smb_mkdir(const char *uri) { return call(uri, do_mkdir, NULL); }

static int do_remove(session *s, const char *share, const char *rest, void *ctx)
{
    int rc = in_share(share, rest);
    if (rc) return rc;
    struct smb2_stat_64 st;
    if ((rc = smb2_stat(s->ctx, rest, &st))) return rc;
    if (st.smb2_type != SMB2_TYPE_DIRECTORY) return smb2_unlink(s->ctx, rest);
    /* rmdir is a delete-on-close open, and Samba removes a full folder that way without reporting an error */
    struct smb2dir *d = smb2_opendir(s->ctx, rest);
    if (!d) return nt_error(s);
    struct smb2dirent *de;
    int empty = 1;
    while (empty && (de = smb2_readdir(s->ctx, d))) empty = !strcmp(de->name, ".") || !strcmp(de->name, "..");
    smb2_closedir(s->ctx, d);
    return empty ? smb2_rmdir(s->ctx, rest) : -ENOTEMPTY;
}

static int smb_remove(const char *uri) { return call(uri, do_remove, NULL); }

static int do_rename(session *s, const char *share, const char *rest, void *ctx)
{
    int rc = in_share(share, rest);
    if (rc) return rc;
    place *to = ctx;
    if (strcmp(to->share, share)) return -EXDEV;
    struct smb2_stat_64 st;
    if (!smb2_stat(s->ctx, to->rest, &st) && strcasecmp(to->rest, rest)) return -EEXIST;
    return smb2_rename(s->ctx, rest, to->rest);
}

static int smb_rename(const char *from, const char *to)
{
    if (!fr_uri_same_backend(from, to)) return -EXDEV;
    place p = place_of(fr_uri_path(to));
    int rc = *p.rest ? call(from, do_rename, &p) : -EPERM;
    free(p.share);
    return rc;
}

static int do_free_space(session *s, const char *share, const char *rest, void *ctx)
{
    if (!*share) return -ENOTSUP;
    struct smb2_statvfs st;
    int rc = smb2_statvfs(s->ctx, rest, &st);
    if (rc) return rc;
    uint64_t *v = ctx;
    v[0] = st.f_bavail * st.f_bsize;
    v[1] = st.f_blocks * st.f_bsize;
    return 0;
}

static int smb_free_space(const char *uri, uint64_t *free_bytes, uint64_t *total_bytes)
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
    struct smb2fh *fh;
} file_stream;

static long st_read(fr_stream *p, void *buf, size_t n)
{
    file_stream *f = (file_stream *)p;
    int rc = fr_conn_relock(f->c, f->generation);
    if (rc) return rc;
    session *s = fr_conn_session(f->c);
    if (n > s->max_read) n = s->max_read;
    rc = checked(s, smb2_read(s->ctx, f->fh, buf, (uint32_t)n));
    fr_conn_unlock(f->c, fr_net_broken(rc));
    return rc;
}

static long st_write(fr_stream *p, const void *buf, size_t n)
{
    file_stream *f = (file_stream *)p;
    int rc = fr_conn_relock(f->c, f->generation);
    if (rc) return rc;
    session *s = fr_conn_session(f->c);
    size_t done = 0;
    while (done < n && rc >= 0) {
        size_t chunk = n - done < s->max_write ? n - done : s->max_write;
        rc = checked(s, smb2_write(s->ctx, f->fh, (const uint8_t *)buf + done, (uint32_t)chunk));
        if (rc > 0) done += (size_t)rc;
        else if (!rc) rc = -EIO;
    }
    fr_conn_unlock(f->c, rc < 0 && fr_net_broken(rc));
    return rc < 0 ? rc : (long)done;
}

static int st_close(fr_stream *p)
{
    file_stream *f = (file_stream *)p;
    int rc = fr_conn_relock(f->c, f->generation);
    if (!rc) {
        session *s = fr_conn_session(f->c);
        rc = checked(s, smb2_close(s->ctx, f->fh));
        fr_conn_unlock(f->c, fr_net_broken(rc));
    }
    fr_conn_unref(f->c);
    free(f);
    return rc;
}

static fr_stream *open_stream(const char *uri, int flags, int *err)
{
    fr_net_uri u;
    if (fr_net_parse(uri, &u)) return *err = -EINVAL, NULL;
    char *key = fr_net_key_share(&u);
    place p = place_of(u.path);
    file_stream *f = NULL;
    *err = in_share(p.share, p.rest);
    for (int attempt = 0; !*err; attempt++) {
        int fresh;
        fr_conn *c = fr_conn_get(key, &u, &ops, &fresh, err);
        if (!c) break;
        session *s = fr_conn_session(c);
        struct smb2fh *fh = smb2_open(s->ctx, p.rest, flags);
        if (fh) {
            f = fr_realloc(NULL, sizeof *f);
            *f = (file_stream){{st_read, st_write, st_close}, c, fr_conn_generation(c), fh};
            fr_conn_unlock(c, 0);
            break;
        }
        *err = nt_error(s);
        int broken = fr_net_broken(*err);
        fr_conn_put(c, broken);
        if (!broken || fresh || attempt) break;
    }
    free(p.share);
    free(key);
    fr_net_uri_free(&u);
    return f ? &f->s : NULL;
}

static fr_stream *smb_open_read(const char *uri, int *err) { return open_stream(uri, O_RDONLY, err); }

static fr_stream *smb_open_write(const char *uri, int exclusive, int *err)
{
    return open_stream(uri, O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC), err);
}

const fr_backend fr_backend_smb = {
    .scheme = "smb",
    .list = smb_list,
    .stat = smb_stat,
    .open_read = smb_open_read,
    .open_write = smb_open_write,
    .mkdir = smb_mkdir,
    .rename = smb_rename,
    .remove = smb_remove,
    .free_space = smb_free_space,
};

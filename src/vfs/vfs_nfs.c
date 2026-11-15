/* nfs://host[:port]/export/path over libnfs, as the process's own uid and gid (AUTH_SYS). The server's exports come
 * from its MOUNT service (NFSv3); each is mounted when first used, and the folders above them (nfs://host/ and on)
 * show the exports' path segments. A server without a MOUNT service, or on another port than 2049, is taken as
 * NFSv4 with everything under its pseudo-root. The pool keeps one session per server with every export it mounted. */
#define _GNU_SOURCE
#include "net/net.h"
#include "vfs.h"
#include <errno.h>
#include <fcntl.h>
#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw-mount.h>
#include <nfsc/libnfs-raw-nfs.h>
#include <nfsc/libnfs-raw-nfs4.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#define TIMEOUT_MS 30000
#define EXPORTS_TIMEOUT_MS 10000
#define CHUNK (1 << 20)

typedef struct {
    char *path;
    struct nfs_context *nfs;
} mount_t;

typedef struct {
    char *host;
    int port, v4;
    fr_strs exports;
    struct {
        mount_t **v; /* pointers, so a mount stays put while a call mounts another */
        int n, cap;
    } mounts;
} session;

static void session_close(void *p)
{
    session *s = p;
    for (int i = 0; i < s->mounts.n; i++) {
        nfs_destroy_context(s->mounts.v[i]->nfs);
        free(s->mounts.v[i]->path);
        free(s->mounts.v[i]);
    }
    free(s->mounts.v);
    fr_strs_free(&s->exports);
    free(s->host);
    free(s);
}

static void *nf_connect(const char *key, const fr_net_uri *u, int *err)
{
    session *s = fr_realloc(NULL, sizeof *s);
    *s = (session){.host = fr_strdup(u->host), .port = u->port};
    /* another port than 2049 can only be NFSv4: v3's MOUNT service is found through the portmapper on the host */
    struct exportnode *list = u->port == 2049 ? mount_getexports_timeout(u->host, EXPORTS_TIMEOUT_MS) : NULL;
    for (struct exportnode *e = list; e; e = e->ex_next) {
        size_t n = strlen(e->ex_dir);
        while (n > 1 && e->ex_dir[n - 1] == '/') n--;
        if (*e->ex_dir == '/') FR_PUSH(s->exports, fr_strndup(e->ex_dir, n));
    }
    if (list) mount_free_export_list(list);
    if (!s->exports.n) {
        s->v4 = 1;
        FR_PUSH(s->exports, fr_strdup("/"));
    }
    return s;
}

static const fr_conn_ops ops = {nf_connect, session_close};

/* where path is: in an export (0; *m mounted, *rel the path inside it, "/" at its root), above the exports (1), or
 * -errno */
static int where(session *s, const char *path, mount_t **m, const char **rel)
{
    const char *best = NULL;
    size_t bn = 0;
    for (int i = 0; i < s->exports.n; i++) {
        const char *e = s->exports.v[i];
        size_t n = strlen(e);
        if (!strcmp(e, "/")) n = 0;
        if ((n == 0 || (!strncmp(path, e, n) && (path[n] == 0 || path[n] == '/'))) && (!best || n > bn)) best = e, bn = n;
    }
    if (!best) {
        size_t pn = strlen(path);
        for (int i = 0; i < s->exports.n; i++)
            if (pn == 1 || (!strncmp(s->exports.v[i], path, pn) && s->exports.v[i][pn] == '/')) return 1;
        return -ENOENT;
    }
    *rel = path[bn] ? path + bn : "/";
    for (int i = 0; i < s->mounts.n; i++)
        if (!strcmp(s->mounts.v[i]->path, best)) return *m = s->mounts.v[i], 0;
    struct nfs_context *nfs = nfs_init_context();
    if (!nfs) return -ENOMEM;
    nfs_set_timeout(nfs, TIMEOUT_MS);
    nfs_set_autoreconnect(nfs, 2);
    if (s->port != 2049) nfs_set_nfsport(nfs, s->port);
    if (s->v4) nfs_set_version(nfs, NFS_V4);
    int rc = nfs_mount(nfs, s->host, best);
    if (rc) {
        nfs_destroy_context(nfs);
        return rc == -ETIMEDOUT ? -EHOSTUNREACH : rc < 0 ? rc : -EIO;
    }
    mount_t *nm = fr_realloc(NULL, sizeof *nm);
    *nm = (mount_t){fr_strdup(best), nfs};
    FR_PUSH(s->mounts, nm);
    *m = nm;
    return 0;
}

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

static void fill(fr_entry *e, const char *name, const struct nfs_stat_64 *st)
{
    e->name = fr_strdup(name);
    e->hidden = name[0] == '.';
    e->is_link = S_ISLNK(st->nfs_mode);
    e->is_dir = S_ISDIR(st->nfs_mode);
    e->size = e->is_dir ? 0 : st->nfs_size;
    e->mtime = (int64_t)st->nfs_mtime;
    e->mode = (uint32_t)(st->nfs_mode & 07777);
}

static char *child_of(const char *dir, const char *name)
{
    return fr_format("%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}

/* a link's target decides whether it shows as a folder, and its size */
static void follow(struct nfs_context *nfs, const char *rel, fr_entry *e)
{
    struct nfs_stat_64 t;
    if (nfs_stat64(nfs, rel, &t)) return;
    e->is_dir = S_ISDIR(t.nfs_mode);
    if (!e->is_dir) e->size = t.nfs_size;
}

static int do_list(session *s, const char *path, void *ctx)
{
    fr_entries *out = ctx, got = {0};
    mount_t *m;
    const char *rel;
    int rc = where(s, path, &m, &rel);
    if (rc == 1) {
        /* above the exports: the next segment of each export under path, once */
        size_t pn = strcmp(path, "/") ? strlen(path) : 0;
        for (int i = 0; i < s->exports.n; i++) {
            const char *e = s->exports.v[i];
            if (strncmp(e, path, pn) || e[pn] != '/') continue;
            const char *seg = e + pn + 1, *end = strchr(seg, '/');
            char *name = fr_strndup(seg, end ? (size_t)(end - seg) : strlen(seg));
            int dup = !*name;
            for (int k = 0; k < got.n && !dup; k++) dup = !strcmp(got.v[k].name, name);
            if (dup) free(name);
            else FR_PUSH(got, ((fr_entry){.name = name, .is_dir = 1, .hidden = name[0] == '.'}));
        }
        *out = got;
        return 0;
    }
    if (rc) return rc;
    struct nfsdir *d;
    if ((rc = nfs_opendir(m->nfs, rel, &d))) return rc;
    struct nfsdirent *de;
    while ((de = nfs_readdir(m->nfs, d))) {
        if (!strcmp(de->name, ".") || !strcmp(de->name, "..")) continue;
        fr_entry e = {.name = fr_strdup(de->name), .hidden = de->name[0] == '.'};
        e.is_link = de->type == NF3LNK || S_ISLNK(de->mode);
        e.is_dir = de->type == NF3DIR || S_ISDIR(de->mode);
        e.size = e.is_dir ? 0 : de->size;
        e.mtime = (int64_t)de->mtime.tv_sec;
        e.mode = de->mode & 07777;
        if (e.is_link) {
            char *child = child_of(rel, de->name);
            follow(m->nfs, child, &e);
            free(child);
        }
        FR_PUSH(got, e);
    }
    nfs_closedir(m->nfs, d);
    *out = got;
    return 0;
}

static int nf_list(const char *uri, fr_entries *out) { return call(uri, do_list, out); }

static int do_stat(session *s, const char *path, void *ctx)
{
    fr_entry *e = ctx;
    const char *slash = strrchr(path, '/'), *name = slash && slash[1] ? slash + 1 : "/";
    mount_t *m;
    const char *rel;
    int rc = where(s, path, &m, &rel);
    if (rc == 1) {
        *e = (fr_entry){.name = fr_strdup(name), .is_dir = 1, .hidden = name[0] == '.'};
        return 0;
    }
    if (rc) return rc;
    struct nfs_stat_64 st;
    if ((rc = nfs_lstat64(m->nfs, rel, &st))) return rc;
    fill(e, name, &st);
    if (e->is_link) follow(m->nfs, rel, e);
    return 0;
}

static int nf_stat(const char *uri, fr_entry *out) { return call(uri, do_stat, out); }

/* path inside an export and not its root */
static int inside(session *s, const char *path, mount_t **m, const char **rel)
{
    int rc = where(s, path, m, rel);
    if (rc == 1) return -EPERM;
    if (!rc && !strcmp(*rel, "/")) return -EPERM;
    return rc;
}

static int do_mkdir(session *s, const char *path, void *ctx)
{
    mount_t *m;
    const char *rel;
    int rc = where(s, path, &m, &rel);
    if (rc == 1 || (!rc && !strcmp(rel, "/"))) return -EEXIST;
    return rc ? rc : nfs_mkdir(m->nfs, rel);
}

static int nf_mkdir(const char *uri) { return call(uri, do_mkdir, NULL); }

static int do_remove(session *s, const char *path, void *ctx)
{
    mount_t *m;
    const char *rel;
    int rc = inside(s, path, &m, &rel);
    if (rc) return rc;
    struct nfs_stat_64 st;
    if ((rc = nfs_lstat64(m->nfs, rel, &st))) return rc;
    return S_ISDIR(st.nfs_mode) ? nfs_rmdir(m->nfs, rel) : nfs_unlink(m->nfs, rel);
}

static int nf_remove(const char *uri) { return call(uri, do_remove, NULL); }

static int do_rename(session *s, const char *path, void *ctx)
{
    mount_t *m, *m2;
    const char *rel, *rel2;
    int rc = inside(s, path, &m, &rel);
    if (!rc) rc = inside(s, ctx, &m2, &rel2);
    if (rc) return rc;
    if (m != m2) return -EXDEV;
    struct nfs_stat_64 st;
    /* NFS replaces what is there; callers decide about conflicts first */
    if (!nfs_lstat64(m->nfs, rel2, &st)) return -EEXIST;
    return nfs_rename(m->nfs, rel, rel2);
}

static int nf_rename(const char *from, const char *to)
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
    mount_t *m;
    const char *rel;
    int rc = where(s, path, &m, &rel);
    if (rc == 1) return -EPERM;
    if (rc) return rc;
    if (w->what == 1) return nfs_chmod(m->nfs, rel, (int)w->mode);
    struct nfs_stat_64 st;
    if ((rc = nfs_stat64(m->nfs, rel, &st))) return rc;
    struct timeval tv[2] = {{(time_t)st.nfs_atime, 0}, {(time_t)w->mtime, 0}};
    return nfs_utimes(m->nfs, rel, tv);
}

static int nf_set_mtime(const char *uri, int64_t mtime) { return call(uri, do_setattr, &(setattr){mtime, 0, 0}); }

static int nf_set_mode(const char *uri, uint32_t mode) { return call(uri, do_setattr, &(setattr){0, mode, 1}); }

static int do_free_space(session *s, const char *path, void *ctx)
{
    mount_t *m;
    const char *rel;
    int rc = where(s, path, &m, &rel);
    if (rc == 1) return -ENOTSUP;
    if (rc) return rc;
    struct nfs_statvfs_64 st;
    if ((rc = nfs_statvfs64(m->nfs, rel, &st))) return rc;
    /* all sizes 0 (seen with nfs-ganesha through libnfs's NFSv4): the server keeps them to itself */
    if (!st.f_blocks) return -ENOTSUP;
    uint64_t *v = ctx;
    v[0] = st.f_bavail * st.f_frsize;
    v[1] = st.f_blocks * st.f_frsize;
    return 0;
}

static int nf_free_space(const char *uri, uint64_t *free_bytes, uint64_t *total_bytes)
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
    char *export; /* the mount the handle is from; the session may have mounted more since */
    struct nfsfh *fh;
} file_stream;

static struct nfs_context *stream_nfs(file_stream *f)
{
    session *s = fr_conn_session(f->c);
    for (int i = 0; i < s->mounts.n; i++)
        if (!strcmp(s->mounts.v[i]->path, f->export)) return s->mounts.v[i]->nfs;
    return NULL;
}

static long st_read(fr_stream *p, void *buf, size_t n)
{
    file_stream *f = (file_stream *)p;
    int rc = fr_conn_relock(f->c, f->generation);
    if (rc) return rc;
    if (n > CHUNK) n = CHUNK;
    rc = nfs_read(stream_nfs(f), f->fh, buf, n);
    fr_conn_unlock(f->c, rc < 0 && fr_net_broken(rc));
    return rc;
}

static long st_write(fr_stream *p, const void *buf, size_t n)
{
    file_stream *f = (file_stream *)p;
    int rc = fr_conn_relock(f->c, f->generation);
    if (rc) return rc;
    struct nfs_context *nfs = stream_nfs(f);
    size_t done = 0;
    while (done < n && rc >= 0) {
        size_t chunk = n - done < CHUNK ? n - done : CHUNK;
        rc = nfs_write(nfs, f->fh, (const char *)buf + done, chunk);
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
        rc = nfs_close(stream_nfs(f), f->fh);
        fr_conn_unlock(f->c, fr_net_broken(rc));
    }
    fr_conn_unref(f->c);
    free(f->export);
    free(f);
    return rc;
}

typedef struct {
    int flags;
    struct nfsfh *fh;
    char *export;
} opening;

static int do_open(session *s, const char *path, void *ctx)
{
    opening *o = ctx;
    mount_t *m;
    const char *rel;
    int rc = inside(s, path, &m, &rel);
    if (rc == -EPERM) rc = -EISDIR;
    if (rc) return rc;
    if ((rc = nfs_open2(m->nfs, rel, o->flags, 0666, &o->fh))) return rc;
    o->export = fr_strdup(m->path);
    return 0;
}

static fr_stream *open_stream(const char *uri, int flags, int *err)
{
    fr_net_uri u;
    if (fr_net_parse(uri, &u)) return *err = -EINVAL, NULL;
    char *key = fr_net_key(&u);
    file_stream *f = NULL;
    for (int attempt = 0;; attempt++) {
        int fresh;
        fr_conn *c = fr_conn_get(key, &u, &ops, &fresh, err);
        if (!c) break;
        opening o = {flags, NULL, NULL};
        *err = do_open(fr_conn_session(c), u.path, &o);
        int broken = fr_net_broken(*err);
        if (!*err) {
            f = fr_realloc(NULL, sizeof *f);
            *f = (file_stream){{st_read, st_write, st_close}, c, fr_conn_generation(c), o.export, o.fh};
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

static fr_stream *nf_open_read(const char *uri, int *err) { return open_stream(uri, O_RDONLY, err); }

static fr_stream *nf_open_write(const char *uri, int exclusive, int *err)
{
    return open_stream(uri, O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC), err);
}

const fr_backend fr_backend_nfs = {
    .scheme = "nfs",
    .list = nf_list,
    .stat = nf_stat,
    .open_read = nf_open_read,
    .open_write = nf_open_write,
    .mkdir = nf_mkdir,
    .rename = nf_rename,
    .remove = nf_remove,
    .set_mtime = nf_set_mtime,
    .free_space = nf_free_space,
    .set_mode = nf_set_mode,
};

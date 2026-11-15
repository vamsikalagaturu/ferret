/* saf://<tree URI, percent-encoded>/<path>: a folder tree granted through the Storage Access Framework, for SD
 * cards and USB drives. All files access reads them through /storage but cannot write there; every change goes
 * through DocumentsContract (Saf.java), and reads take the /storage path when it is readable. */
#include "android.h"
#include "vfs/vfs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXTERNAL_TREE "content://com.android.externalstorage.documents/tree/"

static int unreserved(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || strchr("-._~", c); }

static char *encode(const char *s)
{
    char *out = fr_realloc(NULL, strlen(s) * 3 + 1), *o = out;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (unreserved(*p)) *o++ = (char)*p;
        else o += sprintf(o, "%%%02X", *p);
    *o = 0;
    return out;
}

static int hex(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static char *decode(const char *s, size_t n)
{
    char *out = fr_realloc(NULL, n + 1), *o = out;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '%' && i + 2 < n && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0)
            *o++ = (char)(hex(s[i + 1]) * 16 + hex(s[i + 2])), i += 2;
        else *o++ = s[i];
    *o = 0;
    return out;
}

static char *tree_of(const char *uri)
{
    const char *a = uri + strlen("saf://");
    return decode(a, strcspn(a, "/"));
}

/* the /storage path of a tree on Android's own external storage provider, when this app can read it there */
static char *mirror(const char *tree, const char *path)
{
    if (strncmp(tree, EXTERNAL_TREE, strlen(EXTERNAL_TREE))) return NULL;
    const char *t = tree + strlen(EXTERNAL_TREE);
    char *id = decode(t, strcspn(t, "/")), *colon = strchr(id, ':');
    char *r = NULL;
    if (colon) {
        *colon = 0;
        char *root = !strcmp(id, "primary") ? fr_strdup("/storage/emulated/0") : fr_format("/storage/%s", id);
        if (!access(root, R_OK | X_OK)) r = fr_format("%s%s%s%s", root, colon[1] ? "/" : "", colon + 1, strcmp(path, "/") ? path : "");
        free(root);
    }
    free(id);
    return r;
}

char *saf_local_path(const char *uri)
{
    if (strncmp(uri, "saf://", 6)) return NULL;
    char *tree = tree_of(uri), *m = mirror(tree, fr_uri_path(uri));
    free(tree);
    return m;
}

static char *mirror_uri(const char *uri)
{
    char *tree = tree_of(uri), *m = mirror(tree, fr_uri_path(uri)), *r = m ? fr_uri_from_path(m) : NULL;
    free(tree), free(m);
    return r;
}

/* Saf.java answers "-<errno>" for a failure */
static int status(char *r)
{
    int rc = !r ? -EIO : *r == '-' ? atoi(r) : 0;
    free(r);
    return rc;
}

static char *call(const char *method, const char *uri, const char *extra)
{
    char *tree = tree_of(uri);
    char *r = extra ? java(method, 3, tree, fr_uri_path(uri), extra) : java(method, 2, tree, fr_uri_path(uri));
    free(tree);
    return r;
}

/* ask: without the grant, the system's folder picker opens for it. Only listing asks (opening a folder, not the
 * browser's polling stat, which would reopen the picker right after a cancel), and only on the window's thread, so
 * a job never pops a dialog. */
static int granted_or_ask(const char *uri, int ask)
{
    char *tree = tree_of(uri);
    int ok = status(java("safGranted", 1, tree)) == 0;
    if (!ok && ask && android_main_thread()) free(java("safRequest", 1, tree));
    free(tree);
    return ok;
}

static int granted(const char *uri) { return granted_or_ask(uri, 0); }

/* Saf.java's "d|f\t<size>\t<mtime s>\t<name>" */
static void fill(fr_entry *e, const char *line)
{
    char *p = (char *)line + (*line ? 1 : 0);
    e->is_dir = *line == 'd';
    uint64_t size = *p == '\t' ? strtoull(p + 1, &p, 10) : 0;
    int64_t mtime = *p == '\t' ? strtoll(p + 1, &p, 10) : 0;
    e->size = e->is_dir ? 0 : size;
    e->mtime = mtime;
    e->mode = e->is_dir ? 0775 : 0664;
    e->name = fr_strdup(*p == '\t' ? p + 1 : "");
    e->hidden = e->name[0] == '.';
}

static int saf_list(const char *uri, fr_entries *out)
{
    if (!granted_or_ask(uri, 1)) return -EACCES;
    char *m = mirror_uri(uri);
    if (m) {
        int rc = fr_backend_file.list(m, out);
        free(m);
        if (rc != -EACCES) return rc;
        fr_entries_free(out);
    }
    char *r = call("safList", uri, NULL);
    if (!r || *r == '-') return status(r);
    for (char *line = r, *next; *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        fr_entry e = {0};
        fill(&e, line);
        if (*e.name) FR_PUSH(*out, e);
        else fr_entry_free(&e);
    }
    free(r);
    return 0;
}

static int saf_stat(const char *uri, fr_entry *out)
{
    if (!granted(uri)) return -EACCES;
    char *m = mirror_uri(uri), *name = fr_uri_name(uri);
    int rc = -EACCES;
    if (m) {
        rc = fr_backend_file.stat(m, out);
        free(m);
        if (!rc) free(out->name), out->name = fr_strdup(*name ? name : "/");
    }
    if (rc == -EACCES) {
        char *r = call("safStat", uri, NULL);
        if (r && *r != '-') {
            fill(out, r);
            free(out->name);
            out->name = fr_strdup(*name ? name : "/");
            out->hidden = out->name[0] == '.';
            free(r);
            rc = 0;
        } else rc = status(r);
    }
    free(name);
    return rc;
}

typedef struct {
    fr_stream s;
    int fd;
} fd_stream;

static long fd_read(fr_stream *s, void *buf, size_t n)
{
    for (;;) {
        ssize_t r = read(((fd_stream *)s)->fd, buf, n);
        if (r >= 0) return (long)r;
        if (errno != EINTR) return -errno;
    }
}

static long fd_write(fr_stream *s, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(((fd_stream *)s)->fd, (const char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -errno;
        done += (size_t)r;
    }
    return (long)done;
}

static int fd_close(fr_stream *s)
{
    int rc = close(((fd_stream *)s)->fd) ? -errno : 0;
    free(s);
    return rc;
}

/* mode: "r", "w" (an existing file is truncated) or "x" (-EEXIST when something is there) */
static fr_stream *open_fd(const char *uri, const char *mode, int *err)
{
    char *r = call("safOpen", uri, mode);
    if (!r || *r == '-') return *err = status(r), NULL;
    int fd = atoi(r);
    free(r);
    fd_stream *s = fr_realloc(NULL, sizeof *s);
    *s = (fd_stream){{fd_read, fd_write, fd_close}, fd};
    return &s->s;
}

static fr_stream *saf_open_read(const char *uri, int *err)
{
    if (!granted(uri)) return *err = -EACCES, NULL;
    char *m = mirror_uri(uri);
    if (m) {
        fr_stream *s = fr_backend_file.open_read(m, err);
        free(m);
        if (s || *err != -EACCES) return s;
    }
    return open_fd(uri, "r", err);
}

static fr_stream *saf_open_write(const char *uri, int exclusive, int *err)
{
    if (!granted(uri)) return *err = -EACCES, NULL;
    return open_fd(uri, exclusive ? "x" : "w", err);
}

static int saf_mkdir(const char *uri)
{
    if (!granted(uri)) return -EACCES;
    return status(call("safMkdir", uri, NULL));
}

static int saf_rename(const char *from, const char *to)
{
    if (!granted(from)) return -EACCES;
    /* the cards are FAT: "a" -> "A" finds the source itself at the target, which Saf.java handles */
    char *mf = mirror_uri(from), *mt = mirror_uri(to);
    int taken = 0;
    struct stat a, b;
    if (mf && mt && !lstat(fr_uri_path(mt), &b))
        taken = lstat(fr_uri_path(mf), &a) || a.st_ino != b.st_ino || a.st_dev != b.st_dev;
    free(mf), free(mt);
    if (taken) return -EEXIST;
    return status(call("safRename", from, fr_uri_path(to)));
}

static int saf_remove(const char *uri)
{
    if (!granted(uri)) return -EACCES;
    return status(call("safRemove", uri, NULL));
}

static int saf_free_space(const char *uri, uint64_t *free_bytes, uint64_t *total_bytes)
{
    char *m = mirror_uri(uri);
    if (!m) return -ENOTSUP;
    int rc = fr_backend_file.free_space(m, free_bytes, total_bytes);
    free(m);
    return rc;
}

const fr_backend fr_backend_saf = {
    .scheme = "saf",
    .list = saf_list,
    .stat = saf_stat,
    .open_read = saf_open_read,
    .open_write = saf_open_write,
    .mkdir = saf_mkdir,
    .rename = saf_rename,
    .remove = saf_remove,
    .free_space = saf_free_space,
};

/* Saf.java's volumes(): "<label>\t<tree URI>\n" each */
void saf_places(fr_places *out)
{
    char *r = java("safVolumes", 0);
    for (char *line = r, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *auth = encode(tab + 1);
        FR_PUSH(*out, ((fr_place){fr_strdup(line), fr_format("saf://%s/", auth), fr_strdup("hard-drive"), 1}));
        free(auth);
    }
    free(r);
}

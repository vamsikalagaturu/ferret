#include "vfs.h"
#include "net/net.h"
#include "vfs_archive.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef FR_HAVE_MTP
#include "vfs_mtp.h"
#endif

#ifdef __ANDROID__
extern const fr_backend fr_backend_saf; /* platform/android/saf.c */
#else
extern const fr_backend fr_backend_trash; /* trash_linux.c */
#endif

static const fr_backend *backends[] = {
    &fr_backend_file,
    &fr_backend_archive,
#ifdef __ANDROID__
    &fr_backend_saf,
#else
    &fr_backend_trash,
#endif
#ifdef FR_HAVE_MTP
    &fr_backend_mtp,
#endif
    &fr_backend_smb,
    &fr_backend_sftp,
};

static size_t scheme_len(const char *uri)
{
    const char *p = strstr(uri, "://");
    return p ? (size_t)(p - uri) : 0;
}

const fr_backend *fr_vfs_backend(const char *uri)
{
    size_t n = scheme_len(uri);
    for (size_t i = 0; i < sizeof backends / sizeof *backends; i++)
        if (strlen(backends[i]->scheme) == n && !strncmp(uri, backends[i]->scheme, n)) return backends[i];
    return NULL;
}

#define BACKEND_OR(uri, fail)                                                                                          \
    const fr_backend *b = fr_vfs_backend(uri);                                                                         \
    if (!b) return fail

int fr_vfs_list(const char *uri, fr_entries *out)
{
    BACKEND_OR(uri, -EPROTONOSUPPORT);
    return b->list(uri, out);
}

int fr_vfs_stat(const char *uri, fr_entry *out)
{
    BACKEND_OR(uri, -EPROTONOSUPPORT);
    return b->stat(uri, out);
}

int fr_vfs_exists(const char *uri)
{
    fr_entry e = {0};
    int rc = fr_vfs_stat(uri, &e);
    fr_entry_free(&e);
    return rc == 0;
}

fr_stream *fr_vfs_open_read(const char *uri, int *err)
{
    const fr_backend *b = fr_vfs_backend(uri);
    if (!b) return *err = -EPROTONOSUPPORT, NULL;
    return b->open_read(uri, err);
}

fr_stream *fr_vfs_open_write(const char *uri, int exclusive, int *err)
{
    const fr_backend *b = fr_vfs_backend(uri);
    if (!b) return *err = -EPROTONOSUPPORT, NULL;
    return b->open_write(uri, exclusive, err);
}

int fr_vfs_mkdir(const char *uri)
{
    BACKEND_OR(uri, -EPROTONOSUPPORT);
    return b->mkdir(uri);
}

int fr_vfs_rename(const char *from, const char *to)
{
    if (!fr_uri_same_backend(from, to)) return -EXDEV;
    BACKEND_OR(from, -EPROTONOSUPPORT);
    return b->rename(from, to);
}

int fr_vfs_remove(const char *uri)
{
    BACKEND_OR(uri, -EPROTONOSUPPORT);
    return b->remove(uri);
}

int fr_vfs_trash(const char *uri)
{
    BACKEND_OR(uri, -EPROTONOSUPPORT);
    return b->trash ? b->trash(uri) : -ENOTSUP;
}

int fr_vfs_untrash(const char *uri)
{
    BACKEND_OR(uri, -EPROTONOSUPPORT);
    return b->untrash ? b->untrash(uri) : -ENOTSUP;
}

int fr_vfs_can_trash(const char *uri)
{
    const fr_backend *b = fr_vfs_backend(uri);
    return b && b->trash;
}

int fr_vfs_set_mode(const char *uri, uint32_t mode)
{
    BACKEND_OR(uri, -EPROTONOSUPPORT);
    return b->set_mode ? b->set_mode(uri, mode & 07777) : -ENOTSUP;
}

const char *fr_vfs_error(int err)
{
    switch (-err) {
    case EPROTONOSUPPORT: return "This kind of location is not supported";
    case ENOTSUP: return "Not supported here";
    case FR_EAUTH: return "The server wants a user name and password";
    case FR_EUNTRUSTED: return "The server is not trusted yet";
    case FR_EIDCHANGED: return "The server's identity changed since it was trusted: ferret will not connect (remove "
                               "its line from ~/.config/ferret/known_hosts if the change is expected)";
    case EHOSTUNREACH: return "The server cannot be reached";
    case EBADMSG: return "Not an archive ferret can read, or a damaged one";
    default: return strerror(-err);
    }
}

void fr_entry_free(fr_entry *e)
{
    free(e->name);
    e->name = NULL;
}

void fr_entries_free(fr_entries *e)
{
    for (int i = 0; i < e->n; i++) fr_entry_free(&e->v[i]);
    free(e->v);
    *e = (fr_entries){0};
}

/* ---- URIs ---- */

char *fr_uri_from_path(const char *path) { return fr_format("file://%s", *path == '/' ? path : "/"); }

/* where the path starts: after scheme://authority, or after an archive's "!" */
static const char *path_start(const char *uri)
{
    const char *sep = fr_archive_sep(uri);
    if (sep) return sep + 1;
    size_t n = scheme_len(uri);
    if (!n) return uri;
    const char *p = strchr(uri + n + 3, '/');
    return p ? p : uri + strlen(uri);
}

const char *fr_uri_path(const char *uri)
{
    const char *p = path_start(uri);
    return *p ? p : "/";
}

const char *fr_uri_local(const char *uri) { return strncmp(uri, "file://", 7) ? NULL : fr_uri_path(uri); }

char *fr_uri_join(const char *dir, const char *name)
{
    size_t n = strlen(dir);
    return fr_format("%s%s%s", dir, n && dir[n - 1] == '/' ? "" : "/", name);
}

/* archive://<outer>!/ itself: its outer URI, else NULL */
static char *archive_root_outer(const char *uri)
{
    const char *sep = fr_archive_sep(uri);
    if (!sep) return NULL;
    for (const char *s = sep + 1; *s; s++)
        if (*s != '/') return NULL;
    return fr_archive_outer(uri);
}

char *fr_uri_parent(const char *uri)
{
    char *outer = archive_root_outer(uri);
    if (outer) {
        char *parent = fr_uri_parent(outer);
        free(outer);
        return parent;
    }
    const char *p = path_start(uri);
    size_t n = strlen(uri);
    while (n > (size_t)(p - uri) + 1 && uri[n - 1] == '/') n--;
    if (n <= (size_t)(p - uri) + 1) return NULL;
    while (n > (size_t)(p - uri) && uri[n - 1] != '/') n--;
    if (n > (size_t)(p - uri) + 1) n--;
    return fr_strndup(uri, n);
}

char *fr_uri_name(const char *uri)
{
    char *outer = archive_root_outer(uri);
    if (outer) {
        char *name = fr_uri_name(outer);
        free(outer);
        return name;
    }
    const char *p = path_start(uri);
    size_t n = strlen(uri);
    while (n > (size_t)(p - uri) && uri[n - 1] == '/') n--;
    size_t start = n;
    while (start > (size_t)(p - uri) && uri[start - 1] != '/') start--;
    return fr_strndup(uri + start, n - start);
}

int fr_uri_same_backend(const char *a, const char *b)
{
    const char *pa = path_start(a), *pb = path_start(b);
    return pa - a == pb - b && !strncmp(a, b, (size_t)(pa - a));
}

int fr_uri_within(const char *dir, const char *uri)
{
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') n--;
    if (strncmp(dir, uri, n)) return 0;
    return uri[n] == 0 || uri[n] == '/' || (n && dir[n - 1] == '/');
}

char *fr_uri_free_name(const char *dir, const char *name, int is_dir)
{
    char *uri = fr_uri_join(dir, name);
    if (!fr_vfs_exists(uri)) return uri;
    free(uri);
    size_t stem = fr_stem_len(name, is_dir);
    for (int i = 2;; i++) {
        char *candidate = fr_format("%.*s (%d)%s", (int)stem, name, i, name + stem);
        uri = fr_uri_join(dir, candidate);
        free(candidate);
        if (!fr_vfs_exists(uri)) return uri;
        free(uri);
    }
}

int fr_name_valid(const char *name)
{
    return *name && strcmp(name, ".") && strcmp(name, "..") && !strchr(name, '/');
}

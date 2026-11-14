#ifndef FR_VFS_H
#define FR_VFS_H
/* Every location is a URI, scheme://authority/path, kept unescaped (file:///home/a b.txt). Each scheme has a
 * backend; the calls below pick it by the URI. Errors are negative errno values (0 = done). */
#include "core/util.h"
#include <stdint.h>

typedef struct {
    char *name;
    int is_dir, is_link, hidden;
    uint64_t size;
    int64_t mtime; /* seconds since the epoch */
    uint32_t mode; /* permission bits */
} fr_entry;

typedef struct {
    fr_entry *v;
    int n, cap;
} fr_entries;

typedef struct fr_stream fr_stream;
struct fr_stream {
    long (*read)(fr_stream *s, void *buf, size_t n);        /* bytes, 0 at the end, -errno */
    long (*write)(fr_stream *s, const void *buf, size_t n); /* bytes, -errno */
    int (*close)(fr_stream *s);                             /* frees the stream; -errno when the data did not land */
};

typedef struct {
    const char *scheme;
    int (*list)(const char *uri, fr_entries *out);
    int (*stat)(const char *uri, fr_entry *out); /* without following a last symlink */
    fr_stream *(*open_read)(const char *uri, int *err);
    /* exclusive: -EEXIST when something is there; else an existing file is truncated */
    fr_stream *(*open_write)(const char *uri, int exclusive, int *err);
    int (*mkdir)(const char *uri);
    int (*rename)(const char *from, const char *to); /* -EEXIST if `to` exists, -EXDEV across devices */
    int (*remove)(const char *uri);                  /* a file, a link or an empty folder */
    /* optional (NULL when the storage has none) */
    int (*trash)(const char *uri);
    int (*set_mtime)(const char *uri, int64_t mtime);
    int (*copy_link)(const char *from, const char *to); /* recreates a symlink */
    int (*free_space)(const char *uri, uint64_t *free_bytes, uint64_t *total_bytes);
    int (*set_mode)(const char *uri, uint32_t mode); /* the permission bits (07777), of a symlink's target */
} fr_backend;

const fr_backend *fr_vfs_backend(const char *uri);

int fr_vfs_list(const char *uri, fr_entries *out);
int fr_vfs_stat(const char *uri, fr_entry *out);
int fr_vfs_exists(const char *uri);
fr_stream *fr_vfs_open_read(const char *uri, int *err);
fr_stream *fr_vfs_open_write(const char *uri, int exclusive, int *err);
int fr_vfs_mkdir(const char *uri);
int fr_vfs_rename(const char *from, const char *to);
int fr_vfs_remove(const char *uri);
int fr_vfs_trash(const char *uri); /* -ENOTSUP where there is no trash */
int fr_vfs_can_trash(const char *uri);
int fr_vfs_set_mode(const char *uri, uint32_t mode); /* -ENOTSUP where the storage has no permissions */
const char *fr_vfs_error(int err);

/* ---- permission bits and trees (vfs_tree.c) ---- */
void fr_mode_rwx(uint32_t mode, char out[10]);  /* "rwxr-sr-t", as ls -l writes them */
void fr_mode_octal(uint32_t mode, char out[6]); /* "0755", "4755" */
/* old with the bits in clear taken away and those in set added; a folder inside a tree being changed also gets
 * execute (search) wherever it gets read, so what is below it stays reachable */
uint32_t fr_mode_edit(uint32_t old, uint32_t set, uint32_t clear, int folder_inside);

typedef struct {
    uint64_t bytes;  /* of the files; links are not followed */
    int64_t files;   /* everything that is not a folder, links included */
    int64_t folders;
    int64_t unreadable; /* folders that could not be listed */
} fr_tree_count;
/* adds uri and everything under it to c, which another thread may read meanwhile (__atomic loads); stops early once
 * *cancel is set */
void fr_vfs_measure(const char *uri, fr_tree_count *c, const int *cancel);

void fr_entry_free(fr_entry *e);
void fr_entries_free(fr_entries *e);

/* URIs */
char *fr_uri_from_path(const char *path);  /* "/a/b" -> "file:///a/b" */
const char *fr_uri_path(const char *uri);  /* the path part, "/" at the root */
const char *fr_uri_local(const char *uri); /* the path of a file:// URI, NULL for other schemes */
char *fr_uri_join(const char *dir, const char *name);
char *fr_uri_parent(const char *uri); /* NULL at the root */
char *fr_uri_name(const char *uri);   /* last segment, "" at the root */
int fr_uri_same_backend(const char *a, const char *b);
int fr_uri_within(const char *dir, const char *uri); /* uri is dir or below it */
/* a name that is not taken in dir: "a (2).txt", "a (3).txt", ... (folders without the extension split) */
char *fr_uri_free_name(const char *dir, const char *name, int is_dir);
/* a name a folder can hold: non-empty, not "." or "..", no "/" or NUL */
int fr_name_valid(const char *name);

extern const fr_backend fr_backend_file;
#endif

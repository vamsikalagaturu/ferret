#ifndef FR_VFS_ARCHIVE_H
#define FR_VFS_ARCHIVE_H
/* Archives through libarchive (vfs_archive.c): the archive:// backend (URIs in vfs.h) and the whole-archive
 * operations the jobs use. Read: zip, 7z, tar (plain, gz, bz2, xz, zst), rar, iso, cpio and what else libarchive
 * reads. Written (rewritten whole, to a hidden file beside it that then takes its place): zip, 7z and tar. */
#include "vfs.h"

/* the '!' ending the outer URI of an archive:// URI, NULL for any other URI */
const char *fr_archive_sep(const char *uri);
int fr_archive_is_uri(const char *uri);
char *fr_archive_outer(const char *uri); /* NULL when uri is not inside an archive */
/* the path inside: "docs/x.txt", "" at the root */
char *fr_archive_inner(const char *uri);
char *fr_archive_root(const char *outer); /* "archive://<outer>!/" */

/* by the name: opening it in the list browses into it (zip, tar.*, 7z, rar, iso, cpio; not .apk, .jar, .docx ...) */
int fr_archive_browsable(const char *name);
/* by the name: a kind ferret writes (zip, 7z, tar, tar.gz/bz2/xz/zst) */
int fr_archive_writable(const char *name);
/* the name without its archive extension: "a.tar.gz" -> "a" (a new string) */
char *fr_archive_stem(const char *name);

/* what the whole-archive operations report; any member may be NULL */
typedef struct {
    void *ctx;
    void (*total)(void *ctx, uint64_t bytes, int files); /* adds to what is to be done */
    void (*progress)(void *ctx, uint64_t bytes, int files);
    void (*current)(void *ctx, const char *name);
    void (*fail)(void *ctx, const char *uri, int err); /* one item could not be done; the rest go on */
    const int *cancel;
} fr_archive_hooks;

typedef enum { FR_AR_ADD, FR_AR_DELETE, FR_AR_RENAME, FR_AR_MKDIR } fr_archive_op;
typedef struct {
    fr_archive_op op;
    const char *path; /* inside: ADD and MKDIR where it goes, DELETE what goes (with what is under it), RENAME from */
    const char *src;  /* ADD: a file or folder on any backend, added with what is under it; NULL an empty file */
    const char *to;   /* RENAME: the new path inside */
} fr_archive_change;

/* applies the changes in order to the archive at outer (a file URI on any backend but another archive) in one
 * pass; nothing changes unless all of it landed. 0, -ECANCELED or the first error. */
int fr_archive_rewrite(const char *outer, const fr_archive_change *changes, int n, const fr_archive_hooks *h);
/* a new archive at outer (-EEXIST when taken), its kind by its name, holding sources (URIs) at its root */
int fr_archive_create(const char *outer, const fr_strs *sources, const fr_archive_hooks *h);
/* in one pass, copies from[i] (a path inside, "" the whole archive) with what is under it to the URI to[i]:
 * folders merge, files are replaced. failed[i] (may be NULL) is set when part of item i failed. 0, -ECANCELED or
 * the first error. */
int fr_archive_extract(const char *outer, const char *const *from, const char *const *to, int n, int *failed,
                       const fr_archive_hooks *h);
/* forgets what was read of the archives (tests) */
void fr_archive_cache_clear(void);
#endif

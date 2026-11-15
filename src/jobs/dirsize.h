#ifndef FR_DIRSIZE_H
#define FR_DIRSIZE_H
/* Folders' sizes, everything below each added up, through the VFS (any storage), one folder after another; run with
 * fr_async_run(fr_dirsize_work, done, d), the window reading the counts meanwhile and setting cancel to stop it. */
#include "vfs/vfs.h"

typedef struct {
    fr_strs uris;
    fr_tree_count *counts; /* one per URI: read with __atomic loads while it runs */
    int finished;          /* how many URIs are measured whole (atomic) */
    int cancel;            /* set (atomic) to stop; what was left is not finished */
    unsigned owner;        /* the caller's */
} fr_dirsize;

fr_dirsize *fr_dirsize_new(const fr_strs *uris); /* copies them */
void fr_dirsize_work(void *d);
void fr_dirsize_free(fr_dirsize *d);
#endif

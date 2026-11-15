#ifndef FR_DUPES_H
#define FR_DUPES_H
/* Files with the same content below a folder, through the VFS (any storage). Only files sharing their size with
 * another are read: first SHA-256 of their first FR_DUPES_PREFIX bytes, then, for those still alike and longer than
 * that, SHA-256 of all of them (the order fdupes and rmlint narrow down in). Empty files, links and, unless asked
 * for, hidden items are left out. Run it on a thread of its own: it reads the whole of each candidate. */
#include "vfs/vfs.h"
#include <stdint.h>

#define FR_DUPES_PREFIX (64 * 1024)

enum { FR_DUPES_LISTING, FR_DUPES_HASHING, FR_DUPES_DONE };

typedef struct {
    int phase;
    int64_t files, folders;          /* looked at */
    int64_t unreadable;              /* folders not listed and files not read */
    uint64_t hash_total, hash_done;  /* bytes of the candidates, and of those read or no longer needed */
} fr_dupes_status; /* another thread reads it meanwhile: __atomic loads */

typedef struct {
    fr_entries files; /* their names: paths below the root ("a/b.txt") */
    int *group;       /* per file, its group from 1: a group's files together, the groups biggest file first, each
                       * group's in path order */
    int ngroups;
    uint64_t wasted; /* the bytes of every file but the first of its group */
} fr_dupes;

/* 0; -ECANCELED once *cancel was set (out stays empty); -errno when root cannot be listed */
int fr_dupes_find(const char *root, int hidden, fr_dupes *out, fr_dupes_status *st, const int *cancel);
void fr_dupes_free(fr_dupes *d);
#endif

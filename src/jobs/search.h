#ifndef FR_SEARCH_H
#define FR_SEARCH_H
/* One search at a time below a folder, on a thread of its own so it never waits behind a copy (nor a copy behind
 * it). It goes through the VFS, so any storage can be searched; a local folder is walked with openat instead. */
#include "vfs/vfs.h"
#include <stdint.h>

#define FR_SEARCH_MAX_FILE (50ull << 20) /* a content search skips bigger files */
#define FR_SEARCH_SHOWN 300                /* an indexed name search returns its best this many (found counts all) */

/* Collects every path below a local root on a thread of its own; name searches of that root (and hidden) then match
 * in memory. A new call replaces the index; drop it before the window closes. */
void fr_search_index(const char *root, int hidden);
void fr_search_index_drop(void);

typedef struct {
    int running;
    int64_t folders, files; /* looked at so far */
    int found;
    int unreadable;    /* folders that could not be listed */
    char current[256]; /* the folder being looked in, below the root */
} fr_search_status;

/* Stops a search still running, then looks below root for names matching term (fr_name_matches), or with content,
 * files holding term as plain text (ASCII case ignored; a NUL in the first 8 KB makes a file binary, skipped).
 * hidden: also hidden items and what is inside hidden folders. wake: called from the thread as results come. */
void fr_search_start(const char *root, const char *term, int content, int hidden, void (*wake)(void));
void fr_search_cancel(void);
void fr_search_stop(void); /* cancels and waits for the thread: before the window closes */
/* moves the results found since the last call into out, their names the paths below the root ("a/b.txt"); the
 * status into st when not NULL */
void fr_search_take(fr_entries *out, fr_search_status *st);
void fr_search_wait(void); /* until the search ends (tests) */

/* 1 when the file holds term as fr_search_start() matches it, 0 when not, binary or too big; -errno */
int fr_search_file(const char *uri, const char *term);
#endif

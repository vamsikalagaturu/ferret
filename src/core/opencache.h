#ifndef FR_OPENCACHE_H
#define FR_OPENCACHE_H
/* Local copies of files kept elsewhere (inside an archive, in the trash, on a phone or a server), made so another
 * app can open them: <root>/<key>/<name>, the key a hash of the source's URI, so the same file always lands in the
 * same place and a copy whose size and mtime still match the source's is used again. The copies are read-only
 * snapshots, never written back; the window clears those older than a day when it starts. */
#include <stdint.h>

#define FR_OPENCACHE_MAX_AGE (24 * 60 * 60)

char *fr_opencache_root(void); /* $XDG_CACHE_HOME/ferret/open, else ~/.cache/ferret/open */
/* root/<16 hex digits of uri's SHA-256>/name ("file" for an empty name) */
char *fr_opencache_path(const char *root, const char *uri, const char *name);
/* path is a copy of a source with this size and mtime: 1, else 0 (also for a source without an mtime) */
int fr_opencache_fresh(const char *path, uint64_t size, int64_t mtime);
/* the copy at path was used now: its entry stays another day */
void fr_opencache_touch(const char *path);
/* removes root's entries last used before `before` (seconds since the epoch); how many went */
int fr_opencache_clean(const char *root, int64_t before);
#endif

#ifndef FR_WATCH_H
#define FR_WATCH_H
/* Folders on this machine watched for changes: inotify on the Linux desktop, read on a thread of its own. A burst of
 * changes is told once, FR_WATCH_COALESCE_MS after its first. Where a folder cannot be watched (Android, the watch
 * limit, a network or FUSE filesystem, whose changes elsewhere inotify never hears of) fr_watch_add says so, and the
 * caller polls it instead. */

#define FR_WATCH_COALESCE_MS 100

/* wake: called from the thread after each burst; 0, or -1 when there is no watching here */
int fr_watch_start(void (*wake)(void));
void fr_watch_stop(void); /* no wake() after it returns */
/* a handle (two adds of one folder share it, each to be removed), or -1: poll this folder */
int fr_watch_add(const char *path);
void fr_watch_remove(int handle);
/* grows once per burst of changes in the folder, and when the folder itself went (deleted, moved, unmounted) */
unsigned fr_watch_serial(int handle);
/* the folder went, so the handle hears nothing more: remove it, and add the folder again should it be back */
int fr_watch_gone(int handle);
#endif

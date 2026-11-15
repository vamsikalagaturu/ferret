#ifndef FR_DISKS_VIEW_H
#define FR_DISKS_VIEW_H
/* What disks_view.c (the disk map, the queue) and disks_usage.c (look inside, the storage overview) share. */
#include "app.h"
#include "disks/usage.h"

Color dview_group_color(int group_kind);
char *dview_mount_of(const char *key); /* where the volume key names is mounted now (malloc'd), NULL not mounted */

/* look inside a volume; key names it in the session's cache (a block's object path, or a place's URI) */
int usage_is_open(void);
void usage_open(const char *key, const char *mount_point, const char *title); /* mount_point NULL: mounted read-only first */
void usage_rescan(void);
void usage_close(void);
void usage_draw(Rectangle r);
void usage_keys(void);
void usage_poll(void);
int usage_disk_done(const fr_disk_done *d);
void usage_forget(void);
void usage_shutdown(void);

/* where the platform has no disk manager: the places' volumes, look inside on a tap */
void storage_draw(Rectangle r);
void storage_keys(void);
#endif

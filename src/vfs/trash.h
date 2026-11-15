#ifndef FR_TRASH_H
#define FR_TRASH_H
/* The desktop's trash (trash_linux.c) as trash:///: every trash folder of the user in one listing, the home one and
 * each mount's, with the items' deletion dates as their mtime. Items can be read, copied out and deleted for good;
 * restoring puts them back where they came from. Android has no trash: there these find nothing. */
#include <stdint.h>

#define FR_TRASH_URI "trash:///"

int fr_trash_is_root(const char *uri); /* trash:/// itself */
/* a .trashinfo file's Path (percent-decoded: absolute, or relative to the trash's mount) and DeletionDate in seconds
 * since the epoch (written in local time; -1 when missing); 0, or -1 when it is no trash info */
int fr_trashinfo_parse(const char *text, char **path, int64_t *deleted);
/* where an item of trash:/// came from (an absolute path) and when it was deleted; 0 or -errno */
int fr_trash_origin(const char *uri, char **path, int64_t *deleted);
/* moves an item of trash:/// back where it came from, beside what took its name meanwhile ("a (2).txt"), making
 * the folders that went; *to (may be NULL) gets the file:// URI it landed at. 0 or -errno */
int fr_trash_restore(const char *uri, char **to);
#endif

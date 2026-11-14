#ifndef FR_LOCAL_INFO_H
#define FR_LOCAL_INFO_H
/* What the kernel knows about a local path beyond fr_entry: the properties panel shows it. Linux and Android. */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char point[1024], type[64], source[1024];
} fr_mount;

typedef struct {
    int is_link, is_dir;
    uint32_t mode;      /* of the item itself (a link's is meaningless) */
    uint32_t target_mode; /* is_link: of what it points to, when that exists */
    int target_ok;
    char target[4096];  /* is_link: what it points to, as written */
    int has_birth;      /* the filesystem records when it was made */
    int64_t birth, atime, mtime, ctime;
    uint32_t uid, gid;
    char owner[64], group[64]; /* "" when the id has no name */
    uint64_t inode, links;
    unsigned dev_major, dev_minor;
    int has_mount;
    fr_mount mount;
} fr_local_info;

/* 0, or -errno when the path cannot be looked at */
int fr_local_info_get(const char *path, fr_local_info *out);

/* the mount holding path, an absolute path without symlinks, from the text of /proc/self/mounts: 0, -ENOENT */
int fr_mount_find(const char *table, const char *path, fr_mount *out);
#endif

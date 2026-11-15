#define _GNU_SOURCE
#include "opencache.h"
#include "hash.h"
#include "util.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char *fr_opencache_root(void)
{
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/') return fr_format("%s/ferret/open", xdg);
    return fr_format("%s/.cache/ferret/open", home && *home ? home : "/tmp");
}

char *fr_opencache_path(const char *root, const char *uri, const char *name)
{
    fr_hash h;
    char hex[65];
    fr_hash_init(&h, FR_SHA256);
    fr_hash_update(&h, uri, strlen(uri));
    fr_hash_final_hex(&h, hex);
    return fr_format("%s/%.16s/%s", root, hex, name && *name ? name : "file");
}

int fr_opencache_fresh(const char *path, uint64_t size, int64_t mtime)
{
    struct stat st;
    if (mtime <= 0 || lstat(path, &st) || !S_ISREG(st.st_mode)) return 0;
    return (uint64_t)st.st_size == size && (int64_t)st.st_mtime == mtime;
}

void fr_opencache_touch(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) return;
    char *dir = fr_strndup(path, (size_t)(slash - path));
    utimensat(AT_FDCWD, dir, NULL, 0);
    free(dir);
}

static void remove_tree(int parent, const char *name)
{
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        DIR *d = fdopendir(fd);
        if (!d) close(fd);
        else {
            struct dirent *de;
            while ((de = readdir(d)))
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) remove_tree(dirfd(d), de->d_name);
            closedir(d);
        }
        unlinkat(parent, name, AT_REMOVEDIR);
    } else unlinkat(parent, name, 0);
}

int fr_opencache_clean(const char *root, int64_t before)
{
    DIR *d = opendir(root);
    if (!d) return 0;
    int removed = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) || (int64_t)st.st_mtime >= before) continue;
        remove_tree(dirfd(d), de->d_name);
        removed++;
    }
    closedir(d);
    return removed;
}

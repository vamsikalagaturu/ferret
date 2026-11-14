/* The freedesktop.org Trash specification 1.0 (specifications.freedesktop.org/trash-spec): files on the home
 * trash's device go to $XDG_DATA_HOME/Trash, others to their mount's $topdir/.Trash/$uid (when an admin made a
 * sticky .Trash) or $topdir/.Trash-$uid, each with an info/<name>.trashinfo the desktop's trash can restore from. */
#ifndef __ANDROID__
#define _GNU_SOURCE
#include "core/util.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char *home_trash(void)
{
    const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (data && *data == '/') return fr_format("%s/Trash", data);
    return home ? fr_format("%s/.local/share/Trash", home) : NULL;
}

/* the mount point holding path: the highest ancestor still on its device */
static char *mount_top(const char *path, dev_t dev)
{
    char *top = fr_strdup(path);
    for (;;) {
        char *slash = strrchr(top, '/');
        if (!slash) break;
        char *up = slash == top ? fr_strdup("/") : fr_strndup(top, (size_t)(slash - top));
        struct stat st;
        if (stat(up, &st) || st.st_dev != dev) {
            free(up);
            break;
        }
        free(top);
        top = up;
        if (!strcmp(top, "/")) break;
    }
    return top;
}

static char *top_trash(const char *top)
{
    uid_t uid = getuid();
    const char *sep = strcmp(top, "/") ? "/" : "";
    char *admin = fr_format("%s%s.Trash", top, sep);
    struct stat st;
    if (!lstat(admin, &st) && S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX)) {
        char *mine = fr_format("%s/%u", admin, (unsigned)uid);
        free(admin);
        if (!mkdir(mine, 0700) || errno == EEXIST) return mine;
        free(mine);
    } else free(admin);
    char *own = fr_format("%s%s.Trash-%u", top, sep, (unsigned)uid);
    if (mkdir(own, 0700) && errno != EEXIST) {
        free(own);
        return NULL;
    }
    if (lstat(own, &st) || !S_ISDIR(st.st_mode) || st.st_uid != uid) {
        free(own);
        return NULL;
    }
    return own;
}

static char *escape_path(const char *p)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(p);
    char *r = fr_realloc(NULL, n * 3 + 1), *o = r;
    for (const unsigned char *c = (const unsigned char *)p; *c; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("/-_.~!*'()", *c))
            *o++ = (char)*c;
        else *o++ = '%', *o++ = hex[*c >> 4], *o++ = hex[*c & 15];
    }
    *o = 0;
    return r;
}

int fr_trash_path(const char *path)
{
    struct stat st;
    if (lstat(path, &st)) return -errno;
    char *trash = home_trash();
    struct stat ts;
    const char *rel = NULL;
    char *top = NULL;
    if (trash && !fr_mkdirs(trash) && !stat(trash, &ts) && ts.st_dev == st.st_dev) {
        if (!strncmp(path, trash, strlen(trash)) && path[strlen(trash)] == '/') {
            free(trash);
            return -ENOTSUP;
        }
    } else {
        free(trash);
        top = mount_top(path, st.st_dev);
        trash = top_trash(top);
        if (!trash) {
            free(top);
            return -ENOTSUP;
        }
        rel = path + strlen(top) + (strcmp(top, "/") ? 1 : 0);
    }
    char *files = fr_format("%s/files", trash), *info = fr_format("%s/info", trash);
    int rc = fr_mkdirs(files);
    if (!rc) rc = fr_mkdirs(info);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t stem = fr_stem_len(base, S_ISDIR(st.st_mode));
    char *info_path = NULL, *dest = NULL;
    int fd = -1;
    for (int i = 1; !rc && i < 10000; i++) {
        char *name = i == 1 ? fr_strdup(base) : fr_format("%.*s.%d%s", (int)stem, base, i, base + stem);
        info_path = fr_format("%s/%s.trashinfo", info, name);
        fd = open(info_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            dest = fr_format("%s/%s", files, name);
            free(name);
            break;
        }
        free(name);
        free(info_path);
        info_path = NULL;
        if (errno != EEXIST) rc = -errno;
    }
    if (!rc && fd < 0) rc = -EEXIST;
    if (!rc) {
        char date[32];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
        char *escaped = escape_path(rel ? rel : path);
        char *text = fr_format("[Trash Info]\nPath=%s\nDeletionDate=%s\n", escaped, date);
        size_t len = strlen(text);
        if (write(fd, text, len) != (ssize_t)len) rc = -EIO;
        if (close(fd)) rc = -errno;
        fd = -1;
        free(escaped);
        free(text);
        if (!rc && rename(path, dest)) rc = -errno;
        if (rc) unlink(info_path);
    }
    if (fd >= 0) close(fd);
    free(info_path);
    free(dest);
    free(files);
    free(info);
    free(trash);
    free(top);
    return rc;
}
#endif

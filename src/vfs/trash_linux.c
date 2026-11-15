/* The freedesktop.org Trash specification 1.0 (specifications.freedesktop.org/trash-spec): files on the home
 * trash's device go to $XDG_DATA_HOME/Trash, others to their mount's $topdir/.Trash/$uid (when an admin made a
 * sticky .Trash) or $topdir/.Trash-$uid, each with an info/<name>.trashinfo the desktop's trash can restore from. */
#ifndef __ANDROID__
#define _GNU_SOURCE
#include "core/util.h"
#include <dirent.h>
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

/* the trash for path on device dev: the home trash on its device, else the one of path's mount, whose mount point
 * *top gets (Info's Path= is relative to it); *top is NULL for the home trash */
static char *trash_for(const char *path, dev_t dev, char **top)
{
    *top = NULL;
    char *trash = home_trash();
    struct stat ts;
    if (trash && !fr_mkdirs(trash) && !stat(trash, &ts) && ts.st_dev == dev) return trash;
    free(trash);
    *top = mount_top(path, dev);
    trash = top_trash(*top);
    if (!trash) free(*top), *top = NULL;
    return trash;
}

int fr_trash_path(const char *path)
{
    struct stat st;
    if (lstat(path, &st)) return -errno;
    char *top = NULL, *trash = trash_for(path, st.st_dev, &top);
    if (!trash) return -ENOTSUP;
    if (!top && !strncmp(path, trash, strlen(trash)) && path[strlen(trash)] == '/') {
        free(trash);
        return -ENOTSUP;
    }
    const char *rel = top ? path + strlen(top) + (strcmp(top, "/") ? 1 : 0) : NULL;
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

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* the value of "key=" in an info file's text, %XX decoded, up to the end of its line; NULL when it has none */
static char *info_value(const char *text, const char *key)
{
    size_t kl = strlen(key);
    for (const char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (strncmp(line, key, kl) || line[kl] != '=') continue;
        const char *v = line + kl + 1, *end = strchr(v, '\n');
        if (!end) end = v + strlen(v);
        char *out = fr_realloc(NULL, (size_t)(end - v) + 1), *o = out;
        for (const char *c = v; c < end; c++) {
            int hi, lo;
            if (*c == '%' && c + 2 < end && (hi = hex_digit(c[1])) >= 0 && (lo = hex_digit(c[2])) >= 0)
                *o++ = (char)(hi << 4 | lo), c += 2;
            else *o++ = *c;
        }
        *o = 0;
        return out;
    }
    return NULL;
}

/* The newest entry of path's trash whose Path= is path goes back there, as a desktop's "Restore" does. */
int fr_trash_restore(const char *path)
{
    struct stat st;
    if (!lstat(path, &st)) return -EEXIST;
    const char *slash = strrchr(path, '/');
    if (!slash) return -EINVAL;
    char *parent = slash == path ? fr_strdup("/") : fr_strndup(path, (size_t)(slash - path));
    int rc = stat(parent, &st) ? -errno : 0;
    free(parent);
    if (rc) return rc;
    char *top = NULL, *trash = trash_for(path, st.st_dev, &top);
    if (!trash) return -ENOENT;
    char *info = fr_format("%s/info", trash);
    DIR *d = opendir(info);
    char *best = NULL, *best_date = NULL;
    struct dirent *de;
    while (d && (de = readdir(d))) {
        size_t n = strlen(de->d_name);
        if (n <= 10 || strcmp(de->d_name + n - 10, ".trashinfo")) continue;
        char *file = fr_format("%s/%s", info, de->d_name), text[8192];
        int fd = open(file, O_RDONLY | O_CLOEXEC);
        free(file);
        if (fd < 0) continue;
        ssize_t got = read(fd, text, sizeof text - 1);
        close(fd);
        if (got <= 0) continue;
        text[got] = 0;
        char *p = info_value(text, "Path"), *date = info_value(text, "DeletionDate");
        char *full = !p ? NULL : *p == '/' ? fr_strdup(p) : top ? fr_format("%s%s%s", top, strcmp(top, "/") ? "/" : "", p) : NULL;
        if (full && !strcmp(full, path) && (!best || strcmp(date ? date : "", best_date ? best_date : "") >= 0)) {
            free(best), free(best_date);
            best = fr_strndup(de->d_name, n - 10);
            best_date = date, date = NULL;
        }
        free(p), free(date), free(full);
    }
    if (d) closedir(d);
    rc = -ENOENT;
    if (best) {
        char *from = fr_format("%s/files/%s", trash, best), *info_path = fr_format("%s/%s.trashinfo", info, best);
        rc = rename(from, path) ? -errno : 0;
        if (!rc) unlink(info_path);
        free(from), free(info_path);
    }
    free(best), free(best_date), free(info), free(trash), free(top);
    return rc;
}
#endif

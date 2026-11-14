#define _GNU_SOURCE
#include "local_info.h"
#include "core/util.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

/* one space-separated field of a mounts line, its \ooo escapes (space, tab, newline, backslash) undone */
static const char *field(const char *s, char *out, size_t len)
{
    size_t n = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
        char c = *s++;
        if (c == '\\' && s[0] >= '0' && s[0] <= '7' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' && s[2] <= '7') {
            c = (char)((s[0] - '0') * 64 + (s[1] - '0') * 8 + (s[2] - '0'));
            s += 3;
        }
        if (n + 1 < len) out[n++] = c;
    }
    out[n] = 0;
    return s;
}

int fr_mount_find(const char *table, const char *path, fr_mount *out)
{
    size_t best = 0;
    int found = 0;
    fr_mount m;
    for (const char *line = table; line && *line;) {
        const char *s = field(line, m.source, sizeof m.source);
        s = field(s, m.point, sizeof m.point);
        field(s, m.type, sizeof m.type);
        size_t n = strlen(m.point);
        int within = n && !strncmp(path, m.point, n) && (n == 1 || path[n] == 0 || path[n] == '/');
        if (within && n >= best) best = n, *out = m, found = 1;
        line = strchr(line, '\n');
        if (line) line++;
    }
    return found ? 0 : -ENOENT;
}

static char *read_all(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t n = 0, cap = 16384;
    char *buf = fr_realloc(NULL, cap);
    for (;;) {
        if (n + 1 >= cap) buf = fr_realloc(buf, cap *= 2);
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    return buf;
}

/* path with its folders' links resolved; the last part stays as it is, so a link is looked up where it lies */
static char *resolve_parent(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) return NULL;
    char *dir = slash == path ? fr_strdup("/") : fr_strndup(path, (size_t)(slash - path));
    char *real = realpath(dir, NULL);
    free(dir);
    if (!real) return NULL;
    char *out = slash[1] ? fr_format("%s%s%s", real, strcmp(real, "/") ? "/" : "", slash + 1) : fr_strdup(real);
    free(real);
    return out;
}

int fr_local_info_get(const char *path, fr_local_info *out)
{
    *out = (fr_local_info){0};
    struct statx sx;
    if (!statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_BTIME, &sx)) {
        out->mode = sx.stx_mode & 07777;
        out->is_link = S_ISLNK(sx.stx_mode), out->is_dir = S_ISDIR(sx.stx_mode);
        out->has_birth = (sx.stx_mask & STATX_BTIME) != 0;
        out->birth = sx.stx_btime.tv_sec;
        out->atime = sx.stx_atime.tv_sec, out->mtime = sx.stx_mtime.tv_sec, out->ctime = sx.stx_ctime.tv_sec;
        out->uid = sx.stx_uid, out->gid = sx.stx_gid;
        out->inode = sx.stx_ino, out->links = sx.stx_nlink;
        out->dev_major = sx.stx_dev_major, out->dev_minor = sx.stx_dev_minor;
    } else {
        struct stat st;
        if (lstat(path, &st)) return -errno;
        out->mode = st.st_mode & 07777;
        out->is_link = S_ISLNK(st.st_mode), out->is_dir = S_ISDIR(st.st_mode);
        out->atime = st.st_atime, out->mtime = st.st_mtime, out->ctime = st.st_ctime;
        out->uid = st.st_uid, out->gid = st.st_gid;
        out->inode = st.st_ino, out->links = st.st_nlink;
        out->dev_major = major(st.st_dev), out->dev_minor = minor(st.st_dev);
    }
    struct passwd *pw = getpwuid(out->uid);
    if (pw) snprintf(out->owner, sizeof out->owner, "%s", pw->pw_name);
    struct group *gr = getgrgid(out->gid);
    if (gr) snprintf(out->group, sizeof out->group, "%s", gr->gr_name);
    if (out->is_link) {
        ssize_t n = readlink(path, out->target, sizeof out->target - 1);
        out->target[n > 0 ? n : 0] = 0;
        struct stat t;
        if (!stat(path, &t)) out->target_ok = 1, out->target_mode = t.st_mode & 07777;
    }
    char *real = out->is_link ? resolve_parent(path) : realpath(path, NULL);
    char *table = real ? read_all("/proc/self/mounts") : NULL;
    if (table) out->has_mount = !fr_mount_find(table, real, &out->mount);
    free(table);
    free(real);
    return 0;
}

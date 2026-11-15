#define _GNU_SOURCE
#include "vfs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static void fill(fr_entry *e, const char *name, const struct stat *st, int is_link, int target_is_dir)
{
    e->name = fr_strdup(name);
    e->is_link = is_link;
    e->is_dir = is_link ? target_is_dir : S_ISDIR(st->st_mode);
    e->hidden = name[0] == '.';
    e->size = S_ISREG(st->st_mode) ? (uint64_t)st->st_size : 0;
    e->mtime = (int64_t)st->st_mtime;
    e->mode = (uint32_t)(st->st_mode & 07777);
}

static int file_list(const char *uri, fr_entries *out)
{
    const char *path = fr_uri_path(uri);
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -errno;
    DIR *d = fdopendir(fd);
    if (!d) {
        int e = -errno;
        close(fd);
        return e;
    }
    struct dirent *de;
    errno = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        int is_link = S_ISLNK(st.st_mode), target_dir = 0;
        struct stat target;
        if (is_link && !fstatat(fd, de->d_name, &target, 0)) {
            target_dir = S_ISDIR(target.st_mode);
            if (S_ISREG(target.st_mode)) st.st_size = target.st_size, st.st_mode = (st.st_mode & ~S_IFMT) | S_IFREG;
        }
        fr_entry e = {0};
        fill(&e, de->d_name, &st, is_link, target_dir);
        FR_PUSH(*out, e);
    }
    closedir(d);
    return 0;
}

static int file_stat(const char *uri, fr_entry *out)
{
    const char *path = fr_uri_path(uri);
    struct stat st, target;
    if (lstat(path, &st)) return -errno;
    int is_link = S_ISLNK(st.st_mode), target_dir = is_link && !stat(path, &target) && S_ISDIR(target.st_mode);
    char *name = fr_uri_name(uri);
    fill(out, *name ? name : "/", &st, is_link, target_dir);
    free(name);
    return 0;
}

typedef struct {
    fr_stream s;
    int fd;
} fd_stream;

static long fd_read(fr_stream *s, void *buf, size_t n)
{
    for (;;) {
        ssize_t r = read(((fd_stream *)s)->fd, buf, n);
        if (r >= 0) return (long)r;
        if (errno != EINTR) return -errno;
    }
}

static long fd_write(fr_stream *s, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(((fd_stream *)s)->fd, (const char *)buf + done, n - done);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return -errno;
        done += (size_t)r;
    }
    return (long)done;
}

static int fd_close(fr_stream *s)
{
    int rc = close(((fd_stream *)s)->fd) ? -errno : 0;
    free(s);
    return rc;
}

static fr_stream *wrap(int fd)
{
    fd_stream *s = fr_realloc(NULL, sizeof *s);
    *s = (fd_stream){{fd_read, fd_write, fd_close}, fd};
    return &s->s;
}

static fr_stream *file_open_read(const char *uri, int *err)
{
    int fd = open(fr_uri_path(uri), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return *err = -errno, NULL;
    return wrap(fd);
}

static fr_stream *file_open_write(const char *uri, int exclusive, int *err)
{
    int fd = open(fr_uri_path(uri), O_WRONLY | O_CREAT | O_CLOEXEC | (exclusive ? O_EXCL : O_TRUNC), 0666);
    if (fd < 0) return *err = -errno, NULL;
    return wrap(fd);
}

static int file_mkdir(const char *uri) { return mkdir(fr_uri_path(uri), 0777) ? -errno : 0; }

static int file_rename(const char *from, const char *to)
{
    const char *a = fr_uri_path(from), *b = fr_uri_path(to);
    struct stat st;
    /* rename(2) replaces silently; callers decide about conflicts first. A case-only rename ("a" -> "A") on a
     * case-insensitive filesystem finds the source itself there. */
    if (!lstat(b, &st)) {
        struct stat src;
        if (lstat(a, &src) || src.st_ino != st.st_ino || src.st_dev != st.st_dev) return -EEXIST;
    }
    return rename(a, b) ? -errno : 0;
}

static int file_remove(const char *uri)
{
    const char *path = fr_uri_path(uri);
    struct stat st;
    if (lstat(path, &st)) return -errno;
    return (S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) ? -errno : 0;
}

static int file_set_mtime(const char *uri, int64_t mtime)
{
    struct timespec ts[2] = {{0, UTIME_OMIT}, {(time_t)mtime, 0}};
    return utimensat(AT_FDCWD, fr_uri_path(uri), ts, AT_SYMLINK_NOFOLLOW) ? -errno : 0;
}

static int file_copy_link(const char *from, const char *to)
{
    char target[4096];
    ssize_t n = readlink(fr_uri_path(from), target, sizeof target - 1);
    if (n < 0) return -errno;
    target[n] = 0;
    return symlink(target, fr_uri_path(to)) ? -errno : 0;
}

static int file_free_space(const char *uri, uint64_t *free_bytes, uint64_t *total_bytes)
{
    struct statvfs s;
    if (statvfs(fr_uri_path(uri), &s)) return -errno;
    *free_bytes = (uint64_t)s.f_bavail * s.f_frsize;
    *total_bytes = (uint64_t)s.f_blocks * s.f_frsize;
    return 0;
}

static int file_set_mode(const char *uri, uint32_t mode) { return chmod(fr_uri_path(uri), (mode_t)mode) ? -errno : 0; }

#ifdef __ANDROID__
#define file_trash NULL
#define file_untrash NULL
#else
int fr_trash_path(const char *path);   /* trash_linux.c */
int fr_trash_untrash(const char *path); /* trash_linux.c */
static int file_trash(const char *uri) { return fr_trash_path(fr_uri_path(uri)); }
static int file_untrash(const char *uri) { return fr_trash_untrash(fr_uri_path(uri)); }
#endif

const fr_backend fr_backend_file = {
    .scheme = "file",
    .list = file_list,
    .stat = file_stat,
    .open_read = file_open_read,
    .open_write = file_open_write,
    .mkdir = file_mkdir,
    .rename = file_rename,
    .remove = file_remove,
    .trash = file_trash,
    .set_mtime = file_set_mtime,
    .copy_link = file_copy_link,
    .free_space = file_free_space,
    .set_mode = file_set_mode,
    .untrash = file_untrash,
};

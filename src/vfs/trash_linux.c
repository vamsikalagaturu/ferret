/* The freedesktop.org Trash specification 1.0 (specifications.freedesktop.org/trash-spec): files on the home
 * trash's device go to $XDG_DATA_HOME/Trash, others to their mount's $topdir/.Trash/$uid (when an admin made a
 * sticky .Trash) or $topdir/.Trash-$uid, each with an info/<name>.trashinfo the desktop's trash can restore from. */
#define _GNU_SOURCE
#include "trash.h"
#include "core/util.h"
#include "vfs.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int hex_digit(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static char *unescape_path(const char *s, size_t n)
{
    char *r = fr_realloc(NULL, n + 1), *o = r;
    for (size_t i = 0; i < n; i++) {
        int hi, lo;
        if (s[i] == '%' && i + 2 < n &&(hi = hex_digit(s[i + 1])) >= 0 && (lo = hex_digit(s[i + 2])) >= 0) {
            *o++ = (char)(hi << 4 | lo);
            i += 2;
        } else *o++ = s[i];
    }
    *o = 0;
    return r;
}

int fr_trashinfo_parse(const char *text, char **path, int64_t *deleted)
{
    *path = NULL;
    *deleted = -1;
    int in = 0, found = 0;
    for (const char *p = text; *p;) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        size_t n = (size_t)(end - p);
        if (n && p[n - 1] == '\r') n--;
        if (n && *p == '[') {
            in = n == 12 && !strncmp(p, "[Trash Info]", 12);
            found |= in;
        } else if (in && n > 5 && !strncmp(p, "Path=", 5) && !*path) *path = unescape_path(p + 5, n - 5);
        else if (in && n > 13 && !strncmp(p, "DeletionDate=", 13)) {
            struct tm tm = {0};
            char date[32];
            snprintf(date, sizeof date, "%.*s", (int)(n - 13), p + 13);
            if (sscanf(date, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
                tm.tm_year -= 1900, tm.tm_mon -= 1, tm.tm_isdst = -1;
                *deleted = (int64_t)mktime(&tm);
            }
        }
        p = *end ? end + 1 : end;
    }
    if (found && *path && **path) return 0;
    free(*path);
    *path = NULL;
    return -1;
}

int fr_trash_is_root(const char *uri) { return !strncmp(uri, "trash://", 8) && !strcmp(fr_uri_path(uri), "/"); }

#ifndef __ANDROID__
#include <dirent.h>
#include <pthread.h>
#include <sys/statvfs.h>

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

/* ---- trash:/// ---- */

/* A trash folder: the home one (top NULL, Paths absolute) or a mount's, whose items are named "<key>~<name>" in
 * trash:/// so the same name in two trashes stays apart. */
typedef struct {
    char *dir, *top;
    char key[9];
} tdir;

typedef struct {
    tdir *v;
    int n, cap;
} tdirs;

static void tdir_free(tdir *t)
{
    free(t->dir), free(t->top);
    *t = (tdir){0};
}

static void tdirs_free(tdirs *t)
{
    for (int i = 0; i < t->n; i++) tdir_free(&t->v[i]);
    free(t->v);
    *t = (tdirs){0};
}

static void make_key(tdir *t)
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (const unsigned char *p = (const unsigned char *)t->dir; *p; p++) h = (h ^ *p) * 16777619u;
    snprintf(t->key, sizeof t->key, "%08x", (unsigned)h);
}

static int is_dir_of(const char *path, uid_t owner)
{
    struct stat st;
    return !lstat(path, &st) && S_ISDIR(st.st_mode) && st.st_uid == owner;
}

/* /proc/self/mounts escapes space, tab, newline and backslash as \ooo */
static void unescape_octal(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
            *o++ = (char)((s[1] - '0') << 6 | (s[2] - '0') << 3 | (s[3] - '0'));
            s += 3;
        } else *o++ = *s;
    }
    *o = 0;
}

static void add_trash(tdirs *out, char *dir, const char *top, const char *home_real)
{
    char *real = realpath(dir, NULL);
    int dup = !real || (home_real && !strcmp(real, home_real));
    for (int i = 0; !dup && i < out->n; i++) {
        char *other = realpath(out->v[i].dir, NULL);
        dup = other && !strcmp(other, real);
        free(other);
    }
    free(real);
    if (dup) {
        free(dir);
        return;
    }
    tdir t = {dir, fr_strdup(top), ""};
    make_key(&t);
    FR_PUSH(*out, t);
}

/* the mounts' trash folders of this user; only filesystems on a device, so no network share is waited on */
static void scan_mounts(tdirs *out)
{
    FILE *f = fopen("/proc/self/mounts", "re");
    if (!f) return;
    char *home = home_trash(), *home_real = home ? realpath(home, NULL) : NULL;
    free(home);
    uid_t uid = getuid();
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *source = strtok(line, " "), *top = strtok(NULL, " ");
        if (!source || !top || source[0] != '/' || source[1] == '/') continue;
        unescape_octal(top);
        const char *sep = strcmp(top, "/") ? "/" : "";
        char *admin = fr_format("%s%s.Trash", top, sep);
        struct stat st;
        if (!lstat(admin, &st) && S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX)) {
            char *mine = fr_format("%s/%u", admin, (unsigned)uid);
            if (is_dir_of(mine, uid)) add_trash(out, mine, top, home_real);
            else free(mine);
        }
        free(admin);
        char *own = fr_format("%s%s.Trash-%u", top, sep, (unsigned)uid);
        if (is_dir_of(own, uid)) add_trash(out, own, top, home_real);
        else free(own);
    }
    fclose(f);
    free(home_real);
}

static pthread_mutex_t mounts_lock = PTHREAD_MUTEX_INITIALIZER;
static tdirs mounts_seen;
static double mounts_at;

/* a copy of the mounts' trashes, looked for again when two seconds old (the jobs' thread walks trees through here) */
static tdirs mount_trashes(void)
{
    pthread_mutex_lock(&mounts_lock);
    double now = fr_now();
    if (!mounts_at || now - mounts_at > 2) {
        tdirs_free(&mounts_seen);
        scan_mounts(&mounts_seen);
        mounts_at = now;
    }
    tdirs r = {0};
    for (int i = 0; i < mounts_seen.n; i++) {
        tdir t = {fr_strdup(mounts_seen.v[i].dir), fr_strdup(mounts_seen.v[i].top), ""};
        memcpy(t.key, mounts_seen.v[i].key, sizeof t.key);
        FR_PUSH(r, t);
    }
    pthread_mutex_unlock(&mounts_lock);
    return r;
}

static char *read_small(const char *path)
{
    FILE *f = fopen(path, "re");
    if (!f) return NULL;
    char *text = fr_realloc(NULL, 65537);
    size_t n = fread(text, 1, 65536, f);
    text[n] = 0;
    fclose(f);
    return text;
}

/* the item an URI below trash:/// names: its trash, its name in files/ and what follows it ("" or "/a/b") */
static int resolve(const char *uri, tdir *t, char **name, const char **rest)
{
    const char *p = fr_uri_path(uri);
    while (*p == '/') p++;
    if (!*p) return -EISDIR;
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    *rest = slash ? slash : "";
    *t = (tdir){0};
    if (len > 9 && p[8] == '~') {
        tdirs m = mount_trashes();
        for (int i = 0; i < m.n && !t->dir; i++)
            if (!strncmp(p, m.v[i].key, 8)) *t = m.v[i], m.v[i] = (tdir){0};
        tdirs_free(&m);
        if (t->dir) {
            *name = fr_strndup(p + 9, len - 9);
            return 0;
        }
    }
    t->dir = home_trash();
    if (!t->dir) return -ENOENT;
    *name = fr_strndup(p, len);
    if (!strcmp(*name, ".") || !strcmp(*name, "..")) {
        free(*name);
        tdir_free(t);
        return -ENOENT;
    }
    return 0;
}

/* the file:// URI of what uri names, and of the item's .trashinfo when uri is the item itself */
static char *locate(const char *uri, char **info, int *err)
{
    tdir t;
    char *name;
    const char *rest;
    if (info) *info = NULL;
    if ((*err = resolve(uri, &t, &name, &rest))) return NULL;
    char *real = fr_format("file://%s/files/%s%s", t.dir, name, rest);
    if (info && !*rest) *info = fr_format("%s/info/%s.trashinfo", t.dir, name);
    free(name);
    tdir_free(&t);
    return real;
}

static int64_t deletion_date(const char *info_path)
{
    char *text = read_small(info_path), *path;
    int64_t when = -1;
    if (text && !fr_trashinfo_parse(text, &path, &when)) free(path);
    free(text);
    return when;
}

static void list_one(const tdir *t, fr_entries *out)
{
    char *files = fr_format("%s/files", t->dir);
    DIR *d = opendir(files);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *path = fr_format("%s/%s", files, de->d_name), *info = fr_format("%s/info/%s.trashinfo", t->dir, de->d_name);
        struct stat st, target;
        if (!lstat(path, &st)) {
            fr_entry e = {0};
            e.name = t->top ? fr_format("%s~%s", t->key, de->d_name) : fr_strdup(de->d_name);
            e.is_link = S_ISLNK(st.st_mode);
            int target_ok = e.is_link && !stat(path, &target);
            e.is_dir = e.is_link ? target_ok && S_ISDIR(target.st_mode) : S_ISDIR(st.st_mode);
            e.size = S_ISREG(st.st_mode) ? (uint64_t)st.st_size : target_ok && S_ISREG(target.st_mode) ? (uint64_t)target.st_size : 0;
            e.mode = (uint32_t)(st.st_mode & 07777);
            int64_t when = deletion_date(info);
            e.mtime = when >= 0 ? when : (int64_t)st.st_mtime;
            FR_PUSH(*out, e);
        }
        free(path), free(info);
    }
    if (d) closedir(d);
    free(files);
}

/* the home trash, then the mounts' */
static tdirs all_trashes(void)
{
    tdirs all = {0};
    char *home = home_trash();
    if (home) FR_PUSH(all, ((tdir){home, NULL, ""}));
    tdirs m = mount_trashes();
    for (int i = 0; i < m.n; i++) FR_PUSH(all, m.v[i]);
    free(m.v);
    return all;
}

static int trash_list(const char *uri, fr_entries *out)
{
    if (!fr_trash_is_root(uri)) {
        int err;
        char *real = locate(uri, NULL, &err);
        if (!real) return err;
        err = fr_backend_file.list(real, out);
        free(real);
        return err;
    }
    tdirs all = all_trashes();
    for (int i = 0; i < all.n; i++) list_one(&all.v[i], out);
    tdirs_free(&all);
    return 0;
}

static int trash_stat(const char *uri, fr_entry *out)
{
    if (fr_trash_is_root(uri)) {
        *out = (fr_entry){.name = fr_strdup("Trash"), .is_dir = 1, .mode = 0700};
        tdirs all = all_trashes();
        for (int i = 0; i < all.n; i++)
            for (int k = 0; k < 2; k++) {
                char *sub = fr_format("%s/%s", all.v[i].dir, k ? "info" : "files");
                struct stat st;
                if (!stat(sub, &st) && (int64_t)st.st_mtime > out->mtime) out->mtime = (int64_t)st.st_mtime;
                free(sub);
            }
        tdirs_free(&all);
        return 0;
    }
    int err;
    char *real = locate(uri, NULL, &err);
    if (!real) return err;
    err = fr_backend_file.stat(real, out);
    free(real);
    if (!err) {
        free(out->name);
        out->name = fr_uri_name(uri);
    }
    return err;
}

static fr_stream *trash_open_read(const char *uri, int *err)
{
    char *real = locate(uri, NULL, err);
    if (!real) return NULL;
    fr_stream *s = fr_backend_file.open_read(real, err);
    free(real);
    return s;
}

static fr_stream *trash_open_write(const char *uri, int exclusive, int *err)
{
    *err = -ENOTSUP;
    return NULL;
}

static int trash_mkdir(const char *uri) { return -ENOTSUP; }
static int trash_rename(const char *from, const char *to) { return -ENOTSUP; }

static int trash_remove(const char *uri)
{
    if (fr_trash_is_root(uri)) return -EPERM;
    int err;
    char *info, *real = locate(uri, &info, &err);
    if (!real) return err;
    err = fr_backend_file.remove(real);
    if (!err && info) unlink(info);
    free(real), free(info);
    return err;
}

static int trash_free_space(const char *uri, uint64_t *free_bytes, uint64_t *total_bytes)
{
    int err;
    char *real = fr_trash_is_root(uri) ? NULL : locate(uri, NULL, &err);
    if (!real) return -ENOTSUP;
    err = fr_backend_file.free_space(real, free_bytes, total_bytes);
    free(real);
    return err;
}

const fr_backend fr_backend_trash = {
    .scheme = "trash",
    .list = trash_list,
    .stat = trash_stat,
    .open_read = trash_open_read,
    .open_write = trash_open_write,
    .mkdir = trash_mkdir,
    .rename = trash_rename,
    .remove = trash_remove,
    .free_space = trash_free_space,
};

/* the item's absolute original path, its deletion date, and where it is in files/ and info/ */
static int origin(const char *uri, char **path, int64_t *deleted, char **real, char **info)
{
    tdir t;
    char *name;
    const char *rest;
    int rc = resolve(uri, &t, &name, &rest);
    if (rc) return rc;
    if (*rest) rc = -EINVAL;
    char *info_path = fr_format("%s/info/%s.trashinfo", t.dir, name), *text = rc ? NULL : read_small(info_path), *p = NULL;
    if (!rc && (!text || fr_trashinfo_parse(text, &p, deleted))) rc = text ? -EINVAL : -ENOENT;
    if (!rc) {
        if (*p == '/') *path = p;
        else if (t.top) {
            *path = fr_format("%s%s%s", t.top, strcmp(t.top, "/") ? "/" : "", p);
            free(p);
        } else rc = -EINVAL, free(p);
    }
    if (!rc && real) *real = fr_format("%s/files/%s", t.dir, name);
    if (!rc && info) *info = info_path, info_path = NULL;
    free(info_path), free(text), free(name);
    tdir_free(&t);
    return rc;
}

int fr_trash_origin(const char *uri, char **path, int64_t *deleted) { return origin(uri, path, deleted, NULL, NULL); }

int fr_trash_restore(const char *uri, char **to)
{
    char *path, *real, *info;
    int64_t deleted;
    if (to) *to = NULL;
    int rc = origin(uri, &path, &deleted, &real, &info);
    if (rc) return rc;
    struct stat st;
    if (lstat(real, &st)) rc = -errno;
    char *slash = strrchr(path, '/');
    char *parent = slash == path ? fr_strdup("/") : fr_strndup(path, (size_t)(slash - path));
    if (!rc && !slash[1]) rc = -EINVAL;
    if (!rc) rc = fr_mkdirs(parent);
    char *dest = NULL;
    if (!rc) {
        char *parent_uri = fr_uri_from_path(parent);
        dest = fr_uri_free_name(parent_uri, slash + 1, S_ISDIR(st.st_mode));
        free(parent_uri);
        if (rename(real, fr_uri_path(dest))) rc = -errno;
    }
    if (!rc) unlink(info);
    if (!rc && to) *to = dest, dest = NULL;
    free(dest), free(parent), free(path), free(real), free(info);
    return rc;
}
#else
int fr_trash_origin(const char *uri, char **path, int64_t *deleted) { return -ENOTSUP; }
int fr_trash_restore(const char *uri, char **to) { return -ENOTSUP; }
#endif

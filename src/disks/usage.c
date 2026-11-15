#define _GNU_SOURCE
#include "usage.h"
#include "core/util.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* per folder the largest files are kept by name, the rest summed, so a folder of a million small files costs no memory */
#define KEEP_FILES 20
#define MAX_ROWS 200

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    dk_scan **v;
    int n, cap;
} cache;
static dk_scan *active;
static pthread_t worker;
static int worker_on, worker_done, cancel;
static unsigned next_id;

void dk_usage_lock(void) { pthread_mutex_lock(&lock); }
void dk_usage_unlock(void) { pthread_mutex_unlock(&lock); }

static int stopped(void) { return __atomic_load_n(&cancel, __ATOMIC_RELAXED); }
static void add(uint64_t *v, uint64_t n) { __atomic_add_fetch(v, n, __ATOMIC_RELAXED); }

static void node_free(dk_node *n)
{
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    free(n->kids);
    free(n->name);
    free(n);
}

static dk_node *node_new(const char *name, int dir)
{
    dk_node *n = fr_realloc(NULL, sizeof *n);
    *n = (dk_node){.name = fr_strdup(name), .dir = dir};
    return n;
}

static void kid_add(dk_node *n, dk_node *k)
{
    if (n->nkids == n->cap) {
        n->cap = n->cap ? n->cap * 2 : 8;
        n->kids = fr_realloc(n->kids, sizeof *n->kids * (size_t)n->cap);
    }
    n->kids[n->nkids++] = k;
}

static void scan_free(dk_scan *s)
{
    if (!s) return;
    free(s->key), free(s->title), free(s->mount_point);
    node_free(s->root);
    free(s);
}

static int find_index(const char *key)
{
    for (int i = 0; key && i < cache.n; i++)
        if (!strcmp(cache.v[i]->key, key)) return i;
    return -1;
}

dk_scan *dk_usage_find(const char *key)
{
    int i = find_index(key);
    return i < 0 ? NULL : cache.v[i];
}

static void drop(const char *key)
{
    int i = find_index(key);
    if (i < 0) return;
    scan_free(cache.v[i]);
    cache.v[i] = cache.v[--cache.n];
}

static void join(void)
{
    if (!worker_on) return;
    pthread_join(worker, NULL);
    worker_on = 0;
}

char *dk_usage_cancel(void)
{
    if (!active) return NULL;
    __atomic_store_n(&cancel, 1, __ATOMIC_RELAXED);
    join();
    __atomic_store_n(&cancel, 0, __ATOMIC_RELAXED);
    char *key = fr_strdup(active->key);
    active = NULL;
    drop(key);
    return key;
}

static dk_scan *entry(const char *key, const char *title)
{
    drop(key);
    dk_scan *s = fr_realloc(NULL, sizeof *s);
    *s = (dk_scan){.id = ++next_id, .key = fr_strdup(key), .title = fr_strdup(title ? title : key)};
    FR_PUSH(cache, s);
    return s;
}

dk_scan *dk_usage_mounting(const char *key, const char *title)
{
    free(dk_usage_cancel());
    dk_scan *s = entry(key, title);
    s->status = SCAN_MOUNTING;
    active = s;
    return s;
}

void dk_usage_fail(const char *key, const char *error)
{
    dk_scan *s = dk_usage_find(key);
    if (!s) return;
    s->status = SCAN_ERROR;
    snprintf(s->error, sizeof s->error, "%s", error);
    if (active == s) active = NULL;
}

const char *dk_usage_active(void) { return active ? active->key : NULL; }

/* ---- the scan ---- */

/* inode numbers of files with more than one link, so each counts once */
static struct {
    uint64_t *v;
    size_t n, cap;
} seen;

static int seen_insert(uint64_t ino)
{
    if (seen.n * 2 >= seen.cap) {
        size_t cap = seen.cap ? seen.cap * 2 : 1024;
        uint64_t *v = calloc(cap, sizeof *v);
        if (!v) return 1;
        for (size_t i = 0; i < seen.cap; i++)
            if (seen.v[i])
                for (size_t k = seen.v[i] * 0x9E3779B97F4A7C15ull % cap;; k = (k + 1) % cap)
                    if (!v[k]) {
                        v[k] = seen.v[i];
                        break;
                    }
        free(seen.v);
        seen.v = v, seen.cap = cap;
    }
    uint64_t key = ino + 1;
    for (size_t k = key * 0x9E3779B97F4A7C15ull % seen.cap;; k = (k + 1) % seen.cap) {
        if (seen.v[k] == key) return 0;
        if (!seen.v[k]) {
            seen.v[k] = key;
            seen.n++;
            return 1;
        }
    }
}

typedef struct {
    uint64_t size;
    char *name;
} file_size;

typedef struct {
    file_size *v;
    int n, cap;
} files_t;

static int by_size_desc(const void *a, const void *b)
{
    const file_size *x = a, *y = b;
    return x->size < y->size ? 1 : x->size > y->size ? -1 : 0;
}

static void keep_files(dk_node *n, files_t *f)
{
    if (f->n) qsort(f->v, (size_t)f->n, sizeof *f->v, by_size_desc);
    for (int i = 0; i < f->n; i++) {
        if (i < KEEP_FILES && f->v[i].size > 0) {
            dk_node *k = node_new(f->v[i].name, 0);
            k->size = f->v[i].size, k->files = 1;
            kid_add(n, k);
        } else n->other_size += f->v[i].size, n->other_files++;
        free(f->v[i].name);
    }
    free(f->v);
    *f = (files_t){0};
}

/* one folder's entries: files into f, same-filesystem folders into dirs */
static void list_dir(dk_scan *s, int fd, DIR *dir, dev_t dev, dk_node *n, files_t *f, fr_strs *dirs)
{
    struct dirent *e;
    while (!stopped() && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW)) {
            n->no_access++;
            continue;
        }
        uint64_t bytes = (uint64_t)st.st_blocks * 512;
        if (S_ISDIR(st.st_mode)) {
            /* another filesystem mounted inside this one is not part of it */
            if (st.st_dev == dev) FR_PUSH(*dirs, fr_strdup(e->d_name));
            continue;
        }
        if (st.st_nlink > 1 && !seen_insert((uint64_t)st.st_ino)) continue;
        FR_PUSH(*f, ((file_size){bytes, fr_strdup(e->d_name)}));
        n->size += bytes, n->files++;
        add(&s->scanned, bytes);
        add(&s->files, 1);
    }
}

static dk_node *scan_dir(dk_scan *s, int parent, const char *name, dev_t dev)
{
    dk_node *n = node_new(name, 1);
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR *dir = fd < 0 ? NULL : fdopendir(fd);
    if (!dir) {
        if (fd >= 0) close(fd);
        n->denied = 1, n->no_access = 1;
        return n;
    }
    struct stat st;
    if (!fstat(fd, &st)) n->size += (uint64_t)st.st_blocks * 512, add(&s->scanned, (uint64_t)st.st_blocks * 512);
    files_t f = {0};
    fr_strs dirs = {0};
    list_dir(s, fd, dir, dev, n, &f, &dirs);
    keep_files(n, &f);
    for (int i = 0; i < dirs.n && !stopped(); i++) {
        dk_node *k = scan_dir(s, fd, dirs.v[i], dev);
        n->size += k->size, n->files += k->files, n->no_access += k->no_access;
        kid_add(n, k);
    }
    fr_strs_free(&dirs);
    closedir(dir);
    return n;
}

/* the top level's files and the names of its folders first; then each folder in turn, shown once it is complete */
static void scan_root(dk_scan *s)
{
    int fd = open(s->mount_point, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *dir = fd < 0 ? NULL : fdopendir(fd);
    struct stat rs;
    if (!dir || fstat(fd, &rs)) {
        int err = errno;
        if (dir) closedir(dir);
        else if (fd >= 0) close(fd);
        snprintf(s->error, sizeof s->error, "Cannot read %s: %s", s->mount_point, strerror(err));
        return;
    }
    dk_node top = {.dir = 1, .size = (uint64_t)rs.st_blocks * 512};
    files_t f = {0};
    fr_strs dirs = {0};
    list_dir(s, fd, dir, rs.st_dev, &top, &f, &dirs);
    keep_files(&top, &f);
    dk_usage_lock();
    s->root->size = top.size, s->root->files = top.files, s->root->no_access = top.no_access;
    s->root->other_size = top.other_size, s->root->other_files = top.other_files;
    for (int i = 0; i < top.nkids; i++) kid_add(s->root, top.kids[i]);
    dk_usage_unlock();
    free(top.kids);
    for (int i = 0; i < dirs.n && !stopped(); i++) {
        dk_usage_lock();
        snprintf(s->current, sizeof s->current, "%s", dirs.v[i]);
        dk_usage_unlock();
        dk_node *k = scan_dir(s, fd, dirs.v[i], rs.st_dev);
        dk_usage_lock();
        s->current[0] = 0;
        s->root->size += k->size, s->root->files += k->files, s->root->no_access += k->no_access;
        kid_add(s->root, k);
        dk_usage_unlock();
    }
    fr_strs_free(&dirs);
    closedir(dir);
}

static void *run(void *arg)
{
    scan_root(arg);
    free(seen.v);
    seen.v = NULL, seen.n = seen.cap = 0;
    __atomic_store_n(&worker_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

dk_scan *dk_usage_start(const char *key, const char *title, const char *mount_point)
{
    if (active && active->status == SCAN_SCANNING && !strcmp(active->key, key)) return active;
    free(dk_usage_cancel());
    join();
    dk_scan *s = entry(key, title);
    s->mount_point = fr_strdup(mount_point);
    s->status = SCAN_SCANNING;
    struct statvfs v;
    if (!statvfs(mount_point, &v)) s->used = (uint64_t)(v.f_blocks - v.f_bfree) * v.f_frsize;
    s->root = node_new(s->title, 1);
    active = s;
    worker_done = 0;
    if (pthread_create(&worker, NULL, run, s)) {
        s->status = SCAN_ERROR;
        snprintf(s->error, sizeof s->error, "Cannot start a thread");
        active = NULL;
    } else worker_on = 1;
    return s;
}

char *dk_usage_poll(void)
{
    if (!active || active->status != SCAN_SCANNING || !__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE)) return NULL;
    join();
    dk_scan *s = active;
    active = NULL;
    s->status = s->error[0] ? SCAN_ERROR : SCAN_DONE;
    s->when = time(NULL);
    return fr_strdup(s->key);
}

void dk_usage_forget(void)
{
    free(dk_usage_cancel());
    for (int i = 0; i < cache.n; i++) scan_free(cache.v[i]);
    cache.n = 0;
}

/* ---- the view's rows ---- */

static void row(dk_rows *out, const char *name, uint64_t size, uint64_t files, int kind, int index, uint64_t whole)
{
    dk_row r = {.kind = kind, .index = index, .size = size, .files = files, .share = whole ? (double)size / (double)whole : 0};
    snprintf(r.name, sizeof r.name, "%s", name);
    FR_PUSH(*out, r);
}

static const dk_node *sort_base;
static int by_node_size(const void *a, const void *b)
{
    const dk_node *x = sort_base->kids[*(const int *)a], *y = sort_base->kids[*(const int *)b];
    /* by name among equal sizes: qsort is not stable, and rows that swap between frames take the wrong tap */
    return x->size < y->size ? 1 : x->size > y->size ? -1 : fr_natcmp(x->name, y->name);
}

void dk_usage_rows(const dk_scan *s, const dk_node *at, dk_rows *out)
{
    out->n = 0;
    out->used = out->scanned = out->unaccounted = out->files = out->here = out->here_files = 0;
    if (!s || !s->root) return;
    dk_usage_lock();
    const dk_node *cur = at ? at : s->root;
    int top = cur == s->root;
    uint64_t whole = top && s->used > cur->size ? s->used : cur->size;
    int *order = fr_realloc(NULL, sizeof *order * (size_t)(cur->nkids + 1));
    for (int i = 0; i < cur->nkids; i++) order[i] = i;
    sort_base = cur;
    if (cur->nkids) qsort(order, (size_t)cur->nkids, sizeof *order, by_node_size);
    uint64_t rest_size = 0, rest_files = 0;
    int rest = 0;
    for (int i = 0; i < cur->nkids; i++) {
        const dk_node *n = cur->kids[order[i]];
        if (i >= MAX_ROWS) {
            rest_size += n->size, rest_files += n->files, rest++;
            continue;
        }
        row(out, n->name, n->size, n->files, n->denied ? ROW_DENIED : n->dir ? ROW_DIR : ROW_FILE, order[i], whole);
    }
    free(order);
    char name[64];
    if (rest) {
        snprintf(name, sizeof name, "%d smaller items", rest);
        row(out, name, rest_size, rest_files, ROW_OTHER, -1, whole);
    }
    if (cur->other_files) {
        snprintf(name, sizeof name, "%llu smaller files", (unsigned long long)cur->other_files);
        row(out, name, cur->other_size, cur->other_files, ROW_OTHER, -1, whole);
    }
    uint64_t scanned = __atomic_load_n(&s->scanned, __ATOMIC_RELAXED);
    if (top && s->current[0]) {
        char counting[300];
        snprintf(counting, sizeof counting, "%s  counting...", s->current);
        row(out, counting, scanned > cur->size ? scanned - cur->size : 0, 0, ROW_COUNTING, -1, whole);
    }
    uint64_t unaccounted = s->used > s->root->size ? s->used - s->root->size : 0;
    if (top && s->status == SCAN_DONE && unaccounted) row(out, "not counted", unaccounted, 0, ROW_UNACCOUNTED, -1, whole);
    out->used = s->used, out->scanned = scanned, out->unaccounted = unaccounted;
    out->files = __atomic_load_n(&s->files, __ATOMIC_RELAXED);
    out->here = cur->size, out->here_files = cur->files;
    dk_usage_unlock();
}

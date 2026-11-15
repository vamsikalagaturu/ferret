#define _GNU_SOURCE
#include "search.h"
#include "core/match.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BLOCK (64 * 1024)
#define BINARY_PROBE 8192

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ended = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static int have_thread, cancel;
static fr_search_status status;
static fr_entries found;
static void (*wake_fn)(void);
static double last_wake;

typedef struct {
    char *root, *term, *lower; /* lower: the term in ASCII lower case, for content */
    size_t term_len;
    int content, hidden;
    char *block; /* BLOCK + term_len bytes */
} search;

static int cancelled(void) { return __atomic_load_n(&cancel, __ATOMIC_RELAXED); }

static void wake(int force)
{
    double now = fr_now();
    if (wake_fn && (force || now - last_wake > 1.0 / 15)) {
        last_wake = now;
        wake_fn();
    }
}

static void count(int folders, int files, int unreadable)
{
    pthread_mutex_lock(&lock);
    status.folders += folders, status.files += files, status.unreadable += unreadable;
    pthread_mutex_unlock(&lock);
}

static void looking_in(const char *rel)
{
    pthread_mutex_lock(&lock);
    snprintf(status.current, sizeof status.current, "%s", rel ? rel : "");
    wake(0);
    pthread_mutex_unlock(&lock);
}

static int any_hidden(const char *rel)
{
    for (const char *p = rel; p; p = strchr(p, '/') ? strchr(p, '/') + 1 : NULL)
        if (*p == '.') return 1;
    return 0;
}

/* rel's entry, stat'ed through the VFS, into the results */
static void add(search *s, const char *rel)
{
    char *uri = fr_uri_join(s->root, rel);
    fr_entry e = {0};
    if (!fr_vfs_stat(uri, &e)) {
        free(e.name);
        e.name = fr_strdup(rel);
        e.hidden = any_hidden(rel);
        pthread_mutex_lock(&lock);
        FR_PUSH(found, e);
        status.found++;
        wake(0);
        pthread_mutex_unlock(&lock);
    }
    free(uri);
}

/* ---- content ---- */

static void lower_n(char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] >= 'A' && p[i] <= 'Z') p[i] += 32;
}

/* a match may straddle two reads: the last term_len - 1 bytes are kept in front of the next */
static int holds(search *s, fr_stream *in)
{
    size_t keep = 0, tl = s->term_len;
    int first = 1;
    for (;;) {
        if (cancelled()) return 0;
        long n = in->read(in, s->block + keep, BLOCK);
        if (n <= 0) return 0;
        if (first && memchr(s->block, 0, (size_t)n < BINARY_PROBE ? (size_t)n : BINARY_PROBE)) return 0;
        first = 0;
        lower_n(s->block + keep, (size_t)n);
        size_t len = keep + (size_t)n;
        if (memmem(s->block, len, s->lower, tl)) return 1;
        keep = tl - 1 < len ? tl - 1 : len;
        memmove(s->block, s->block + len - keep, keep);
    }
}

typedef struct {
    fr_stream s;
    int fd;
} fd_stream;

static long fd_read(fr_stream *st, void *buf, size_t n)
{
    for (;;) {
        ssize_t r = read(((fd_stream *)st)->fd, buf, n);
        if (r >= 0 || errno != EINTR) return r < 0 ? -errno : (long)r;
    }
}

static int local_holds(search *s, int dirfd, const char *name)
{
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return 0;
    fd_stream st = {{fd_read, NULL, NULL}, fd};
    int r = holds(s, &st.s);
    close(fd);
    return r;
}

static int vfs_holds(search *s, const char *uri)
{
    int err = 0;
    fr_stream *in = fr_vfs_open_read(uri, &err);
    if (!in) return 0;
    int r = holds(s, in);
    in->close(in);
    return r;
}

static void init_content(search *s, const char *term)
{
    s->term_len = strlen(term);
    s->lower = fr_strdup(term);
    lower_n(s->lower, s->term_len);
    s->block = fr_realloc(NULL, BLOCK + s->term_len);
}

int fr_search_file(const char *uri, const char *term)
{
    if (!*term) return 0;
    fr_entry e = {0};
    int rc = fr_vfs_stat(uri, &e);
    int big = !rc && (e.is_dir || e.size > FR_SEARCH_MAX_FILE);
    fr_entry_free(&e);
    if (rc) return rc;
    if (big) return 0;
    fr_stream *in = fr_vfs_open_read(uri, &rc);
    if (!in) return rc;
    search s = {0};
    init_content(&s, term);
    int r = holds(&s, in);
    in->close(in);
    free(s.lower), free(s.block);
    return r;
}

/* ---- the name index ---- */

/* Every path below one local root, collected once by a thread of its own (fr_search_index); a name search of that
 * root then scores the paths in memory instead of walking the disk per term, and looks up size and date only for
 * the best FR_SEARCH_SHOWN. Guarded by lock, grown while it is built; grew signals each batch. */
static struct {
    char *root;
    int hidden, building, done, stop;
    struct ix_entry {
        char *rel;
        int base; /* where the name starts in rel */
    } *v;
    int n, cap;
    pthread_t thread;
    int have_thread;
} ix;
static pthread_cond_t grew = PTHREAD_COND_INITIALIZER;

static void ix_walk(int dirfd, const char *rel, int at_slash);

static void ix_add(const char *rel, int base)
{
    pthread_mutex_lock(&lock);
    if (ix.n == ix.cap) ix.cap = ix.cap ? ix.cap * 2 : 4096, ix.v = fr_realloc(ix.v, sizeof *ix.v * (size_t)ix.cap);
    ix.v[ix.n++] = (struct ix_entry){fr_strdup(rel), base};
    if (!(ix.n & 1023)) pthread_cond_broadcast(&grew);
    pthread_mutex_unlock(&lock);
}

/* the kernel's own trees, not files anyone looks for, and endless */
static int pseudo(const char *rel) { return !strcmp(rel, "proc") || !strcmp(rel, "sys") || !strcmp(rel, "dev") || !strcmp(rel, "run"); }

static void ix_walk(int dirfd, const char *rel, int at_slash)
{
    DIR *d = fdopendir(dirfd);
    if (!d) {
        close(dirfd);
        return;
    }
    struct dirent *de;
    while (!__atomic_load_n(&ix.stop, __ATOMIC_RELAXED) && (de = readdir(d))) {
        const char *name = de->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || (name[0] == '.' && !ix.hidden)) continue;
        int type = de->d_type;
        if (type == DT_UNKNOWN) {
            struct stat st;
            if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW)) continue;
            type = S_ISDIR(st.st_mode) ? DT_DIR : DT_REG;
        }
        char *krel = rel ? fr_format("%s/%s", rel, name) : fr_strdup(name);
        ix_add(krel, rel ? (int)strlen(rel) + 1 : 0);
        if (type == DT_DIR && !(at_slash && pseudo(name))) {
            int fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) ix_walk(fd, krel, 0);
        }
        free(krel);
    }
    closedir(d);
}

static void *ix_run(void *arg)
{
    const char *path = arg;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) ix_walk(fd, NULL, !strcmp(path, "/"));
    pthread_mutex_lock(&lock);
    ix.building = 0, ix.done = !ix.stop;
    pthread_cond_broadcast(&grew);
    pthread_mutex_unlock(&lock);
    return NULL;
}

void fr_search_index_drop(void)
{
    __atomic_store_n(&ix.stop, 1, __ATOMIC_RELAXED);
    if (ix.have_thread) pthread_join(ix.thread, NULL);
    ix.have_thread = 0;
    for (int i = 0; i < ix.n; i++) free(ix.v[i].rel);
    free(ix.v), free(ix.root);
    ix = (typeof(ix)){0};
}

void fr_search_index(const char *root, int hidden)
{
    const char *path = fr_uri_local(root);
    if (!path) return;
    fr_search_stop();
    fr_search_index_drop();
    ix.root = fr_strdup(root), ix.hidden = hidden, ix.building = 1;
    if (pthread_create(&ix.thread, NULL, ix_run, (void *)fr_uri_local(ix.root))) ix.building = 0;
    else ix.have_thread = 1;
}

static int ix_serves(const search *s)
{
    return !s->content && ix.root && !strcmp(ix.root, s->root) && ix.hidden == s->hidden && (ix.building || ix.done);
}

typedef struct {
    int i, score;
} scored;

static int by_score(const void *a, const void *b)
{
    const scored *x = a, *y = b;
    return x->score != y->score ? y->score - x->score : x->i - y->i;
}

/* scores the index as it grows (waiting for the build), then keeps the best FR_SEARCH_SHOWN */
static void ix_search(search *s)
{
    int glob = strpbrk(s->term, "*?") != NULL, at = 0, total = 0;
    struct {
        scored *v;
        int n, cap;
    } hits = {0};
    for (;;) {
        pthread_mutex_lock(&lock);
        while (at == ix.n && ix.building && !cancelled()) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 100 * 1000000L;
            if (t.tv_nsec >= 1000000000L) t.tv_sec++, t.tv_nsec -= 1000000000L;
            pthread_cond_timedwait(&grew, &lock, &t);
        }
        /* in batches under the lock, as the build may move the array; it waits a few milliseconds at most */
        int end = ix.n < at + 8192 ? ix.n : at + 8192, more = ix.building || end < ix.n;
        for (; at < end; at++) {
            const char *name = ix.v[at].rel + ix.v[at].base;
            int score = glob ? (fr_glob_match(s->term, name) ? 1 : -1) : fr_fuzzy_score(s->term, name);
            if (score < 0) continue;
            total++;
            FR_PUSH(hits, ((scored){at, score}));
        }
        status.files = ix.n;
        pthread_mutex_unlock(&lock);
        if (cancelled() || !more) break;
        wake(0);
    }
    if (!cancelled() && hits.n) {
        qsort(hits.v, (size_t)hits.n, sizeof *hits.v, by_score);
        for (int k = 0; k < hits.n && k < FR_SEARCH_SHOWN && !cancelled(); k++) {
            pthread_mutex_lock(&lock);
            char *rel = fr_strdup(ix.v[hits.v[k].i].rel);
            pthread_mutex_unlock(&lock);
            add(s, rel);
            free(rel);
        }
        pthread_mutex_lock(&lock);
        status.found = total;
        pthread_mutex_unlock(&lock);
    }
    free(hits.v);
}

/* ---- walking ---- */

static char *child_rel(const char *rel, const char *name) { return rel ? fr_format("%s/%s", rel, name) : fr_strdup(name); }

static void walk_vfs(search *s, const char *uri, const char *rel)
{
    fr_entries kids = {0};
    if (fr_vfs_list(uri, &kids)) {
        count(0, 0, 1);
        return;
    }
    looking_in(rel);
    count(1, 0, 0);
    for (int i = 0; i < kids.n && !cancelled(); i++) {
        fr_entry *k = &kids.v[i];
        if (k->hidden && !s->hidden) continue;
        char *krel = child_rel(rel, k->name), *kuri = fr_uri_join(uri, k->name);
        if (!k->is_dir) count(0, 1, 0);
        if (!s->content ? fr_name_matches(s->term, k->name) : !k->is_dir && k->size <= FR_SEARCH_MAX_FILE && vfs_holds(s, kuri))
            add(s, krel);
        if (k->is_dir && !k->is_link) walk_vfs(s, kuri, krel);
        free(krel), free(kuri);
    }
    fr_entries_free(&kids);
}

static void walk_local(search *s, int dirfd, const char *rel, int at_slash)
{
    DIR *d = fdopendir(dirfd);
    if (!d) {
        close(dirfd);
        count(0, 0, 1);
        return;
    }
    looking_in(rel);
    count(1, 0, 0);
    struct dirent *de;
    while (!cancelled() && (de = readdir(d))) {
        const char *name = de->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || (name[0] == '.' && !s->hidden)) continue;
        int type = de->d_type;
        struct stat st;
        if (type == DT_UNKNOWN) {
            if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW)) continue;
            type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISLNK(st.st_mode) ? DT_LNK : S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN;
        }
        char *krel = child_rel(rel, name);
        if (type != DT_DIR) count(0, 1, 0);
        int match = 0;
        if (!s->content) match = fr_name_matches(s->term, name);
        else if ((type == DT_REG || type == DT_LNK) && !fstatat(dirfd, name, &st, 0) && S_ISREG(st.st_mode) &&
                 (uint64_t)st.st_size <= FR_SEARCH_MAX_FILE)
            match = local_holds(s, dirfd, name);
        if (match) add(s, krel);
        if (type == DT_DIR && !(at_slash && pseudo(name))) {
            int fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) walk_local(s, fd, krel, 0);
            else count(0, 0, 1);
        }
        free(krel);
    }
    closedir(d);
}

static void *run(void *arg)
{
    search *s = arg;
    const char *path = fr_uri_local(s->root);
    pthread_mutex_lock(&lock);
    int indexed = ix_serves(s);
    pthread_mutex_unlock(&lock);
    if (indexed) ix_search(s);
    else if (path) {
        int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) walk_local(s, fd, NULL, !strcmp(path, "/"));
        else count(0, 0, 1);
    } else walk_vfs(s, s->root, NULL);
    pthread_mutex_lock(&lock);
    status.running = 0;
    status.current[0] = 0;
    wake(1);
    pthread_cond_broadcast(&ended);
    pthread_mutex_unlock(&lock);
    free(s->root), free(s->term), free(s->lower), free(s->block);
    free(s);
    return NULL;
}

void fr_search_cancel(void) { __atomic_store_n(&cancel, 1, __ATOMIC_RELAXED); }

void fr_search_stop(void)
{
    fr_search_cancel();
    if (have_thread) pthread_join(thread, NULL);
    have_thread = 0;
    pthread_mutex_lock(&lock);
    fr_entries_free(&found);
    pthread_mutex_unlock(&lock);
}

void fr_search_start(const char *root, const char *term, int content, int hidden, void (*on_found)(void))
{
    fr_search_stop();
    search *s = fr_realloc(NULL, sizeof *s);
    *s = (search){.root = fr_strdup(root), .term = fr_strdup(term), .content = content, .hidden = hidden};
    if (content) init_content(s, term);
    pthread_mutex_lock(&lock);
    status = (fr_search_status){.running = 1};
    wake_fn = on_found;
    cancel = 0;
    pthread_mutex_unlock(&lock);
    if (!*term || pthread_create(&thread, NULL, run, s)) {
        pthread_mutex_lock(&lock);
        status.running = 0;
        pthread_mutex_unlock(&lock);
        free(s->root), free(s->term), free(s->lower), free(s->block), free(s);
        return;
    }
    have_thread = 1;
}

void fr_search_take(fr_entries *out, fr_search_status *st)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < found.n; i++) FR_PUSH(*out, found.v[i]);
    found.n = 0;
    if (st) *st = status;
    pthread_mutex_unlock(&lock);
}

void fr_search_wait(void)
{
    pthread_mutex_lock(&lock);
    while (status.running) pthread_cond_wait(&ended, &lock);
    pthread_mutex_unlock(&lock);
}

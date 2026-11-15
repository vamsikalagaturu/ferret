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

/* Every path below one local root, collected by a thread of its own (fr_search_index); a name search of that root
 * then scores the paths in memory instead of walking the disk per term, and looks up size and date only for the
 * best FR_SEARCH_SHOWN. A refresh builds a second index while searches keep using the finished one, and replaces
 * it when done; a search holds the index it reads (users), and one replaced is freed by its last user. All fields
 * under lock; grew signals each batch added and each build's end. */
typedef struct {
    char *rel;
    int base; /* where the name starts in rel */
} ix_entry;

typedef struct {
    char *root;
    int hidden, building, done, stop, users, retired;
    ix_entry *v;
    int n, cap;
} ix_index;

static ix_index *ix_live, *ix_next; /* searched; being built to replace it (NULL when none) */
static void ix_save(const ix_index *x);
static void ix_release(ix_index *x);
static pthread_cond_t grew = PTHREAD_COND_INITIALIZER;

static void ix_free(ix_index *x)
{
    for (int i = 0; i < x->n; i++) free(x->v[i].rel);
    free(x->v), free(x->root), free(x);
}

static void ix_add(ix_index *x, const char *rel, int base)
{
    pthread_mutex_lock(&lock);
    if (x->n == x->cap) x->cap = x->cap ? x->cap * 2 : 4096, x->v = fr_realloc(x->v, sizeof *x->v * (size_t)x->cap);
    x->v[x->n++] = (ix_entry){fr_strdup(rel), base};
    if (!(x->n & 1023)) pthread_cond_broadcast(&grew);
    pthread_mutex_unlock(&lock);
}

/* the kernel's own trees, not files anyone looks for, and endless */
static int pseudo(const char *rel) { return !strcmp(rel, "proc") || !strcmp(rel, "sys") || !strcmp(rel, "dev") || !strcmp(rel, "run"); }

/* The walk reads folders on IX_WORKERS threads at once: each read waits on the storage (on Android, a round trip
 * through its FUSE layer), so one thread at a time leaves the walk waiting, not working. Measured on a phone's
 * internal storage (44k entries, 2026-11-15): 1 thread 8.5-9.8 s, 2 to 8 threads 0.8-2.5 s with no clear best. The
 * folders still to read are a shared stack; a worker that finds it empty waits until no worker is reading. */
#define IX_WORKERS 4

typedef struct {
    ix_index *x;
    int rootfd, at_slash;
    pthread_mutex_t m;
    pthread_cond_t more;
    char **todo; /* folders below the root still to read; NULL is the root itself */
    int ntodo, cap, reading;
} ix_walk_state;

static void ix_push(ix_walk_state *w, char *rel)
{
    if (w->ntodo == w->cap) w->cap = w->cap ? w->cap * 2 : 256, w->todo = fr_realloc(w->todo, sizeof *w->todo * (size_t)w->cap);
    w->todo[w->ntodo++] = rel;
}

static void ix_read(ix_walk_state *w, const char *rel)
{
    ix_index *x = w->x;
    int fd = rel ? openat(w->rootfd, rel, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : dup(w->rootfd);
    DIR *d = fd >= 0 ? fdopendir(fd) : NULL;
    if (!d) {
        if (fd >= 0) close(fd);
        return;
    }
    struct dirent *de;
    while (!__atomic_load_n(&x->stop, __ATOMIC_RELAXED) && (de = readdir(d))) {
        const char *name = de->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || (name[0] == '.' && !x->hidden)) continue;
        int type = de->d_type;
        if (type == DT_UNKNOWN) {
            struct stat st;
            if (fstatat(dirfd(d), name, &st, AT_SYMLINK_NOFOLLOW)) continue;
            type = S_ISDIR(st.st_mode) ? DT_DIR : DT_REG;
        }
        char *krel = rel ? fr_format("%s/%s", rel, name) : fr_strdup(name);
        ix_add(x, krel, rel ? (int)strlen(rel) + 1 : 0);
        if (type == DT_DIR && !(!rel && w->at_slash && pseudo(name))) {
            pthread_mutex_lock(&w->m);
            ix_push(w, krel);
            pthread_cond_signal(&w->more);
            pthread_mutex_unlock(&w->m);
        } else free(krel);
    }
    closedir(d);
}

static void *ix_worker(void *arg)
{
    ix_walk_state *w = arg;
    pthread_mutex_lock(&w->m);
    for (;;) {
        while (!w->ntodo && w->reading) pthread_cond_wait(&w->more, &w->m);
        if (!w->ntodo || __atomic_load_n(&w->x->stop, __ATOMIC_RELAXED)) break;
        char *rel = w->todo[--w->ntodo];
        w->reading++;
        pthread_mutex_unlock(&w->m);
        ix_read(w, rel);
        free(rel);
        pthread_mutex_lock(&w->m);
        w->reading--;
        pthread_cond_broadcast(&w->more);
    }
    pthread_cond_broadcast(&w->more);
    pthread_mutex_unlock(&w->m);
    return NULL;
}

/* builds x; when it replaces a finished index, that one goes once no search reads it */
static void *ix_run(void *arg)
{
    ix_index *x = arg;
    const char *path = fr_uri_local(x->root);
    ix_walk_state w = {.x = x, .rootfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC), .at_slash = !strcmp(path, "/")};
    if (w.rootfd >= 0) {
        pthread_mutex_init(&w.m, NULL);
        pthread_cond_init(&w.more, NULL);
        ix_push(&w, NULL);
        pthread_t t[IX_WORKERS];
        int started = 0;
        for (int i = 0; i < IX_WORKERS; i++)
            if (!pthread_create(&t[started], NULL, ix_worker, &w)) started++;
        if (!started) ix_worker(&w);
        for (int i = 0; i < started; i++) pthread_join(t[i], NULL);
        for (int i = 0; i < w.ntodo; i++) free(w.todo[i]);
        free(w.todo);
        pthread_cond_destroy(&w.more);
        pthread_mutex_destroy(&w.m);
        close(w.rootfd);
    }
    ix_index *old = NULL;
    pthread_mutex_lock(&lock);
    /* a root that could not be opened gives no index rather than an empty one in place of a good one */
    if (w.rootfd < 0) x->stop = 1;
    x->building = 0, x->done = !x->stop;
    if (x->stop) {
        if (ix_next == x) ix_next = NULL;
        if (ix_live == x) ix_live = NULL;
        x->retired = 1;
    } else if (ix_next == x) {
        old = ix_live, ix_live = x, ix_next = NULL;
        if (old) old->retired = 1;
    }
    int free_x = x->retired && !x->users, free_old = old && !old->users, save = x->done && !x->retired;
    if (save) x->users++;
    pthread_cond_broadcast(&grew);
    pthread_mutex_unlock(&lock);
    if (free_old) ix_free(old);
    if (free_x) ix_free(x);
    if (save) {
        ix_save(x);
        ix_release(x);
    }
    return NULL;
}

/* ---- the index kept on disk ---- */

/* The last finished index, so a search right after start has every name: ~/.cache/ferret/search-index (the app's
 * cache folder on a phone). A first line naming the format, the root and the hidden setting, then each path ended
 * by a NUL. Written beside and renamed over, so a reader never sees half a file. */
#define IX_FILE_HEAD "ferret search index 1"

static char *ix_file(void)
{
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/') return fr_format("%s/ferret/search-index", xdg);
    return fr_format("%s/.cache/ferret/search-index", home && *home ? home : "/tmp");
}

static void ix_save(const ix_index *x)
{
    char *path = ix_file(), *tmp = fr_format("%s.new", path), *dir = fr_strdup(path);
    *strrchr(dir, '/') = 0;
    fr_mkdirs(dir);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fprintf(f, "%s\n%s\n%d\n", IX_FILE_HEAD, x->root, x->hidden) > 0;
    for (int i = 0; ok && i < x->n; i++) ok = fwrite(x->v[i].rel, strlen(x->v[i].rel) + 1, 1, f) == 1;
    if (f && fclose(f)) ok = 0;
    if (ok) ok = !rename(tmp, path);
    if (!ok) unlink(tmp);
    free(path), free(tmp), free(dir);
}

/* the saved index of root and hidden, finished; NULL when there is none for them */
static ix_index *ix_load(const char *root, int hidden)
{
    char *path = ix_file();
    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return NULL;
    char head[64], saved_root[4096];
    int saved_hidden = -1;
    int ok = fgets(head, sizeof head, f) && !strcmp(head, IX_FILE_HEAD "\n") && fgets(saved_root, sizeof saved_root, f) &&
             fscanf(f, "%d", &saved_hidden) == 1 && fgetc(f) == '\n';
    if (ok) saved_root[strcspn(saved_root, "\n")] = 0;
    if (!ok || strcmp(saved_root, root) || saved_hidden != hidden) {
        fclose(f);
        return NULL;
    }
    ix_index *x = fr_realloc(NULL, sizeof *x);
    *x = (ix_index){.root = fr_strdup(root), .hidden = hidden, .done = 1};
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getdelim(&line, &cap, '\0', f)) > 1) {
        if (x->n == x->cap) x->cap = x->cap ? x->cap * 2 : 4096, x->v = fr_realloc(x->v, sizeof *x->v * (size_t)x->cap);
        const char *slash = strrchr(line, '/');
        x->v[x->n++] = (ix_entry){fr_strdup(line), slash ? (int)(slash - line) + 1 : 0};
    }
    free(line);
    fclose(f);
    return x;
}

/* published as the live index, or as the next one, before its builder starts: a build that finishes at once must
 * find itself in place to swap in */
static void ix_start(const char *root, int hidden, int as_next)
{
    ix_index *x = fr_realloc(NULL, sizeof *x);
    *x = (ix_index){.root = fr_strdup(root), .hidden = hidden, .building = 1};
    pthread_mutex_lock(&lock);
    if (as_next) ix_next = x;
    else ix_live = x;
    pthread_mutex_unlock(&lock);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int failed = pthread_create(&t, &attr, ix_run, x);
    pthread_attr_destroy(&attr);
    if (failed) {
        pthread_mutex_lock(&lock);
        if (ix_next == x) ix_next = NULL;
        if (ix_live == x) ix_live = NULL;
        pthread_mutex_unlock(&lock);
        ix_free(x);
    }
}

/* stops the builds and waits for them; a search still reading the live index frees it */
void fr_search_index_drop(void)
{
    pthread_mutex_lock(&lock);
    ix_index *xs[] = {ix_live, ix_next};
    for (int i = 0; i < 2; i++)
        if (xs[i]) __atomic_store_n(&xs[i]->stop, 1, __ATOMIC_RELAXED);
    while ((ix_live && ix_live->building) || (ix_next && ix_next->building)) pthread_cond_wait(&grew, &lock);
    ix_index *live = ix_live;
    ix_live = ix_next = NULL;
    int free_live = live && !live->users;
    if (live) live->retired = 1;
    pthread_mutex_unlock(&lock);
    if (free_live) ix_free(live);
}

void fr_search_index(const char *root, int hidden)
{
    if (!fr_uri_local(root)) return;
    pthread_mutex_lock(&lock);
    int same = ix_live && !strcmp(ix_live->root, root) && ix_live->hidden == hidden, busy = ix_next || (ix_live && ix_live->building);
    pthread_mutex_unlock(&lock);
    if (same && busy) return;
    if (!same) {
        fr_search_index_drop();
        /* the saved one answers at once while the walk refreshes it */
        ix_index *saved = ix_load(root, hidden);
        if (saved) {
            pthread_mutex_lock(&lock);
            ix_live = saved;
            pthread_mutex_unlock(&lock);
            same = 1;
        }
    }
    ix_start(root, hidden, same);
}

/* the index a name search of s->root reads, held (ix_release), NULL when there is none */
static ix_index *ix_take(const search *s)
{
    pthread_mutex_lock(&lock);
    ix_index *x = ix_live;
    if (s->content || !x || strcmp(x->root, s->root) || x->hidden != s->hidden || x->stop) x = NULL;
    if (x) x->users++;
    pthread_mutex_unlock(&lock);
    return x;
}

static void ix_release(ix_index *x)
{
    pthread_mutex_lock(&lock);
    int last = !--x->users && x->retired;
    pthread_mutex_unlock(&lock);
    if (last) ix_free(x);
}

typedef struct {
    int i, score;
} scored;

static int by_score(const void *a, const void *b)
{
    const scored *x = a, *y = b;
    return x->score != y->score ? y->score - x->score : x->i - y->i;
}

/* scores the index as it grows (waiting for a first build), then keeps the best FR_SEARCH_SHOWN */
static void ix_search(search *s, ix_index *x)
{
    int glob = strpbrk(s->term, "*?") != NULL, at = 0, total = 0;
    struct {
        scored *v;
        int n, cap;
    } hits = {0};
    for (;;) {
        pthread_mutex_lock(&lock);
        while (at == x->n && x->building && !cancelled()) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            t.tv_nsec += 100 * 1000000L;
            if (t.tv_nsec >= 1000000000L) t.tv_sec++, t.tv_nsec -= 1000000000L;
            pthread_cond_timedwait(&grew, &lock, &t);
        }
        /* in batches under the lock, as the build may move the array; it waits a few milliseconds at most */
        int end = x->n < at + 8192 ? x->n : at + 8192, more = x->building || end < x->n;
        for (; at < end; at++) {
            const char *name = x->v[at].rel + x->v[at].base;
            int score = glob ? (fr_glob_match(s->term, name) ? 1 : -1) : fr_fuzzy_score(s->term, name);
            if (score < 0) continue;
            total++;
            FR_PUSH(hits, ((scored){at, score}));
        }
        status.files = x->n;
        pthread_mutex_unlock(&lock);
        if (cancelled() || !more) break;
        wake(0);
    }
    if (!cancelled() && hits.n) {
        qsort(hits.v, (size_t)hits.n, sizeof *hits.v, by_score);
        for (int k = 0; k < hits.n && k < FR_SEARCH_SHOWN && !cancelled(); k++) {
            pthread_mutex_lock(&lock);
            char *rel = fr_strdup(x->v[hits.v[k].i].rel);
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
    ix_index *x = ix_take(s);
    if (x) {
        ix_search(s, x);
        ix_release(x);
    } else if (path) {
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

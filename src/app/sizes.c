/* Folder sizes on demand (ranger's dc): measured on a thread through the VFS, kept by URI until the window closes.
 * One goes stale when the folder's own mtime moves, and once a job finishes, for the folders shown and those above
 * them, which the job may have changed below. */
#include "app.h"
#include "jobs/async.h"
#include "jobs/dirsize.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *uri;
    unsigned hash;
    int64_t mtime;
    uint64_t bytes;
} known;

static struct {
    known *v;
    int n, cap;
} cache;

typedef struct {
    fr_dirsize *d;
    int64_t *mtimes; /* each folder's when asked for */
    char *parent;    /* the folder listing them */
} task;

static struct {
    task *v;
    int n, cap;
} tasks;

static unsigned hash_of(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static int find(const char *uri, unsigned hash)
{
    for (int i = 0; i < cache.n; i++)
        if (cache.v[i].hash == hash && !strcmp(cache.v[i].uri, uri)) return i;
    return -1;
}

static void forget(int i)
{
    free(cache.v[i].uri);
    cache.v[i] = cache.v[--cache.n];
}

static void remember(const char *uri, int64_t mtime, uint64_t bytes)
{
    unsigned hash = hash_of(uri);
    int i = find(uri, hash);
    if (i < 0) {
        FR_PUSH(cache, ((known){.uri = fr_strdup(uri), .hash = hash}));
        i = cache.n - 1;
    }
    cache.v[i].mtime = mtime, cache.v[i].bytes = bytes;
}

void sizes_apply(void)
{
    if (!br.uri) return;
    for (int i = 0; i < br.items.n; i++) {
        fr_item *it = &br.items.v[i];
        it->sized = 0, it->tree_size = 0;
        if (!cache.n || !it->e.is_dir || it->placeholder) continue;
        char *uri = fr_uri_join(br.uri, it->e.name);
        int k = find(uri, hash_of(uri));
        free(uri);
        if (k < 0) continue;
        if (cache.v[k].mtime != it->e.mtime) forget(k);
        else it->sized = 1, it->tree_size = cache.v[k].bytes;
    }
}

/* every pane, as br in turn */
static void each_browser(void (*fn)(void))
{
    fr_browser *was = br_cur;
    unsigned salt = ui.id_salt;
    for (int i = 0; i < tabs.n; i++)
        for (int s = 0; s < 2; s++)
            if (tabs.v[i].pane[s]) browser_enter(tabs.v[i].pane[s]), fn();
    br_cur = was;
    ui.id_salt = salt;
}

static void apply_and_sort(void)
{
    sizes_apply();
    if (br.sort == SORT_SIZE) browser_rebuild_view();
    ui.dirty = 1;
}

static void done(void *arg)
{
    fr_dirsize *d = arg;
    int at = -1;
    for (int i = 0; i < tasks.n; i++)
        if (tasks.v[i].d == d) at = i;
    if (at < 0) {
        fr_dirsize_free(d);
        return;
    }
    task t = tasks.v[at];
    memmove(tasks.v + at, tasks.v + at + 1, sizeof *tasks.v * (size_t)(tasks.n - at - 1));
    tasks.n--;
    int finished = __atomic_load_n(&d->finished, __ATOMIC_ACQUIRE);
    for (int i = 0; i < finished; i++) remember(d->uris.v[i], t.mtimes[i], d->counts[i].bytes);
    int64_t unreadable = 0;
    for (int i = 0; i < finished; i++) unreadable += d->counts[i].unreadable;
    if (unreadable && !d->cancel) ui_toast("%lld %s could not be read: the sizes leave them out", (long long)unreadable, unreadable == 1 ? "folder" : "folders");
    each_browser(apply_and_sort);
    fr_dirsize_free(d);
    free(t.mtimes);
    free(t.parent);
}

static task *task_of(unsigned owner)
{
    for (int i = 0; i < tasks.n; i++)
        if (tasks.v[i].d->owner == owner && !tasks.v[i].d->cancel) return &tasks.v[i];
    return NULL;
}

int sizes_running(void) { return task_of(br.id) != NULL; }

void sizes_measure(int all)
{
    if (!br.uri) return;
    task *running = task_of(br.id);
    if (running) {
        __atomic_store_n(&running->d->cancel, 1, __ATOMIC_RELAXED);
        ui_toast("Stopped measuring");
        ui.dirty = 1;
        return;
    }
    fr_item *cur = browser_item(br.cursor);
    int selection = ui.touch || !cur || cur->selected;
    fr_strs uris = {0};
    struct {
        int64_t *v;
        int n, cap;
    } mtimes = {0};
    for (int pass = 0; pass < 2 && !uris.n; pass++)
        for (int v = 0; v < br.view.n; v++) {
            fr_item *it = browser_item(v);
            if (!it->e.is_dir || it->placeholder) continue;
            if (!pass && !all && !(selection ? it->selected : v == br.cursor)) continue;
            FR_PUSH(uris, fr_uri_join(br.uri, it->e.name));
            FR_PUSH(mtimes, it->e.mtime);
        }
    if (!uris.n) {
        ui_toast("No folders here to measure");
        return;
    }
    net_prepare(br.uri);
    fr_dirsize *d = fr_dirsize_new(&uris);
    d->owner = br.id;
    FR_PUSH(tasks, ((task){d, mtimes.v, fr_strdup(br.uri)}));
    if (uris.n > 1) ui_toast("Measuring %d folders", uris.n);
    fr_strs_free(&uris);
    fr_async_run(fr_dirsize_work, done, d);
    ui.dirty = 1;
}

int sizes_progress(const fr_item *it, uint64_t *bytes)
{
    if (!tasks.n || !it->e.is_dir || !br.uri) return 0;
    char *uri = NULL;
    int r = 0;
    for (int t = 0; t < tasks.n && !r; t++) {
        fr_dirsize *d = tasks.v[t].d;
        if (d->cancel || strcmp(tasks.v[t].parent, br.uri)) continue;
        if (!uri) uri = fr_uri_join(br.uri, it->e.name);
        int finished = __atomic_load_n(&d->finished, __ATOMIC_ACQUIRE);
        for (int i = finished; i < d->uris.n && !r; i++)
            if (!strcmp(d->uris.v[i], uri)) {
                r = i == finished ? 1 : 2;
                *bytes = __atomic_load_n(&d->counts[i].bytes, __ATOMIC_RELAXED);
            }
    }
    free(uri);
    return r;
}

/* the cache entries at br's folder or above it; in results (whose files lie deeper), those below it too */
static void forget_shown(void)
{
    if (!br.uri) return;
    for (int i = cache.n - 1; i >= 0; i--)
        if (fr_uri_within(cache.v[i].uri, br.uri) || (br.results && fr_uri_within(br.uri, cache.v[i].uri))) forget(i);
}

void sizes_poll(void)
{
    static int seen_finished;
    int f = fr_jobs_finished();
    if (f != seen_finished) {
        seen_finished = f;
        if (cache.n) {
            each_browser(forget_shown);
            each_browser(apply_and_sort);
        }
    }
    if (tasks.n) ui_redraw_in(0.25);
}

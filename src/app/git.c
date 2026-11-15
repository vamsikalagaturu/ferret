#include "app.h"
#include "jobs/gitstatus.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define GIT_TIMEOUT_MS 3000
/* a work tree changes without its folder's mtime moving (edits inside files, git commands elsewhere) */
#define GIT_REFRESH_S 5.0

/* git runs on a thread of its own per request; the window may drop it before it ends */
typedef struct {
    char *dir;
    fr_git_marks marks;
    int rc, done, refs;
} git_task;

typedef struct {
    unsigned id; /* the browser's */
    char *uri;
    unsigned loads;
    int jobs_seen;
    double at;
    int rc; /* of the last run: -ENOENT no work tree, -ETIMEDOUT, ... */
    git_task *task;
    fr_git_marks marks;
} pane_git;

static struct {
    pane_git *v;
    int n, cap;
} panes;

static void task_drop(git_task *t)
{
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL)) return;
    free(t->dir);
    fr_git_marks_free(&t->marks);
    free(t);
}

static void *task_run(void *arg)
{
    git_task *t = arg;
    t->rc = fr_git_status(t->dir, GIT_TIMEOUT_MS, &t->marks);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    plat_wake();
    task_drop(t);
    return NULL;
}

static void pane_clear(pane_git *p)
{
    if (p->task) task_drop(p->task);
    p->task = NULL;
    fr_git_marks_free(&p->marks);
    free(p->uri);
    p->uri = NULL;
}

static void start(pane_git *p, const fr_browser *b)
{
    git_task *t = fr_realloc(NULL, sizeof *t);
    *t = (git_task){.dir = fr_strdup(fr_uri_local(b->uri)), .refs = 2};
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, task_run, t)) t->rc = -EAGAIN, t->done = 1, t->refs = 1;
    pthread_attr_destroy(&attr);
    p->task = t;
    p->loads = b->loads;
    p->jobs_seen = fr_jobs_finished();
    p->at = ui.now;
}

static int wanted(const fr_browser *b) { return settings.git_status && !ui.touch && b->uri && !b->results && fr_uri_local(b->uri); }

static void poll_one(fr_browser *b)
{
    pane_git *p = NULL;
    for (int i = 0; i < panes.n && !p; i++)
        if (panes.v[i].id == b->id) p = &panes.v[i];
    if (!wanted(b)) {
        if (p) pane_clear(p);
        return;
    }
    if (!p) {
        FR_PUSH(panes, ((pane_git){.id = b->id}));
        p = &panes.v[panes.n - 1];
    }
    if (p->task && __atomic_load_n(&p->task->done, __ATOMIC_ACQUIRE)) {
        p->rc = p->task->rc;
        fr_git_marks_free(&p->marks);
        if (!p->rc) p->marks = p->task->marks, p->task->marks = (fr_git_marks){0};
        task_drop(p->task);
        p->task = NULL;
        if (p->rc == -ETIMEDOUT) ui_toast("git status took too long here; no git marks for this folder");
        ui.dirty = 1;
    }
    if (p->task) return;
    int moved = !p->uri || strcmp(p->uri, b->uri);
    if (moved) {
        fr_git_marks_free(&p->marks);
        free(p->uri);
        p->uri = fr_strdup(b->uri);
        p->rc = 0;
        start(p, b);
        return;
    }
    /* outside a work tree, or where git was too slow, only a change of the folder asks again */
    int changed = p->loads != b->loads || p->jobs_seen != fr_jobs_finished();
    /* by age only on a frame drawn anyway: an idle window runs no git */
    if (changed || (!p->rc && ui.now - p->at >= GIT_REFRESH_S)) start(p, b);
}

void git_poll(void)
{
    for (int i = 0; i < panes.n;) {
        if (browser_by_id(panes.v[i].id)) {
            i++;
            continue;
        }
        pane_clear(&panes.v[i]);
        panes.v[i] = panes.v[--panes.n];
    }
    if (!tabs.n) return;
    const fr_tab *t = &tabs.v[tabs.cur];
    for (int s = 0; s < 2; s++)
        if (t->pane[s]) poll_one(t->pane[s]);
}

int git_flags(const char *name)
{
    for (int i = 0; i < panes.n; i++) {
        pane_git *p = &panes.v[i];
        if (p->id != br.id) continue;
        if (!p->uri || !br.uri || strcmp(p->uri, br.uri) || !settings.git_status) return 0;
        return fr_git_flags(&p->marks, name);
    }
    return 0;
}

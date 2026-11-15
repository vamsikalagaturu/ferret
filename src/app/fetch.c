/* Opening files that are not on this device (inside an archive, in the trash, on a phone or a server): each is
 * copied into the open cache (core/opencache.h) by a job, which shows its progress in the job strip and can be
 * cancelled there, then the copies go to the default app, or to Open with. */
#include "app.h"
#include "core/opencache.h"
#include "jobs/async.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int id; /* its job; 0 for a file on this device, which needs none */
    char *path;
    int ok;
} fetch;

typedef struct {
    int with; /* to Open with, else each to its default app */
    struct {
        fetch *v;
        int n, cap;
    } files;
    int left;
} group;

static struct {
    group *v;
    int n, cap;
} groups;
static int told; /* that the copies are snapshots, once a run */

static void clean_work(void *arg)
{
    char *root = fr_opencache_root();
    fr_opencache_clean(root, (int64_t)time(NULL) - FR_OPENCACHE_MAX_AGE);
    free(root);
}

static void clean_done(void *arg) {}

void fetch_start(void) { fr_async_run(clean_work, clean_done, NULL); }

static void finish(group *g);

void fetch_open(const fr_strs *uris, int with)
{
    char *root = fr_opencache_root();
    group g = {.with = with};
    for (int i = 0; i < uris->n; i++) {
        const char *uri = uris->v[i], *local = fr_uri_local(uri);
        if (local) {
            FR_PUSH(g.files, ((fetch){0, fr_strdup(local), 1}));
            continue;
        }
        net_prepare(uri);
        char *name = fr_uri_name(uri), *path = fr_opencache_path(root, uri, name), *copy = fr_uri_from_path(path);
        FR_PUSH(g.files, ((fetch){fr_job_submit_fetch(uri, copy), path, 0}));
        g.left++;
        free(name), free(copy);
    }
    free(root);
    if (g.left) FR_PUSH(groups, g);
    else finish(&g);
}

static void finish(group *g)
{
    fr_strs paths = {0};
    for (int i = 0; i < g->files.n; i++)
        if (g->files.v[i].ok) {
            FR_PUSH(paths, g->files.v[i].path);
            g->files.v[i].path = NULL;
        }
    int copies = 0;
    for (int i = 0; i < g->files.n; i++) copies += g->files.v[i].id && g->files.v[i].ok;
    if (paths.n && g->with) ops_open_with_paths(&paths);
    else
        for (int i = 0; i < paths.n; i++) {
            const char *slash = strrchr(paths.v[i], '/');
            if (plat_open(paths.v[i])) ui_toast("No app can open %s", slash ? slash + 1 : paths.v[i]), copies = 0;
        }
    if (copies && !told) {
        told = 1;
        ui_toast("Opened a read-only copy: changes to it are not saved back");
    }
    fr_strs_free(&paths);
    for (int i = 0; i < g->files.n; i++) free(g->files.v[i].path);
    free(g->files.v);
}

int fetch_job_done(const fr_job_status *st)
{
    if (st->kind != JOB_FETCH) return 0;
    for (int k = 0; k < groups.n; k++) {
        group *g = &groups.v[k];
        for (int i = 0; i < g->files.n; i++) {
            if (g->files.v[i].id != st->id) continue;
            g->files.v[i].ok = st->state == JOB_DONE && !st->failed;
            if (st->state == JOB_CANCELLED) ui_toast("Cancelled");
            else if (st->failed) ui_toast("Could not open %s", st->error);
            if (--g->left > 0) return 1;
            finish(g);
            memmove(groups.v + k, groups.v + k + 1, sizeof *groups.v * (size_t)(groups.n - k - 1));
            groups.n--;
            return 1;
        }
    }
    return 1;
}

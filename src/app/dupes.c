/* Duplicate files below the folder, found on a thread (jobs/dupes.h) and shown as the folder's results, a group's
 * files together, so the list's selection, delete and trash work on them as on any search's results. */
#include "app.h"
#include "jobs/async.h"
#include "jobs/dupes.h"
#include "jobs/search.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *root;
    int hidden, cancel, rc;
    fr_dupes_status st;
    fr_dupes out;
} task;

/* one run at a time, into the browser that started it; its numbers stay for the bar once it is done */
typedef struct {
    task *t;
    unsigned owner;
    int ngroups, nfiles, rc;
    int64_t unreadable;
    uint64_t wasted;
} run;
static run D;

static void work(void *arg)
{
    task *t = arg;
    t->rc = fr_dupes_find(t->root, t->hidden, &t->out, &t->st, &t->cancel);
}

static void task_free(task *t)
{
    fr_dupes_free(&t->out);
    free(t->root);
    free(t);
}

static void deliver(task *t)
{
    D.rc = t->rc, D.ngroups = t->out.ngroups, D.nfiles = t->out.files.n, D.wasted = t->out.wasted;
    D.unreadable = t->st.unreadable;
    if (t->rc && t->rc != -ECANCELED) ui_toast("Could not look for duplicates: %s", fr_vfs_error(t->rc));
    if (t->rc || !t->out.files.n) return;
    int base = br.items.n, n = t->out.files.n;
    browser_results_add(&t->out.files);
    for (int i = 0; i < n && base + i < br.items.n; i++) br.items.v[base + i].group = t->out.group[i];
    browser_rebuild_view();
}

static void done(void *arg)
{
    task *t = arg;
    if (t == D.t) {
        D.t = NULL;
        fr_browser *b = browser_by_id(D.owner);
        if (b && b->dupes && b->uri && !strcmp(b->uri, t->root)) {
            fr_browser *was = br_cur;
            unsigned salt = ui.id_salt;
            browser_enter(b);
            deliver(t);
            br_cur = was;
            ui.id_salt = salt;
        }
        ui.dirty = 1;
    }
    task_free(t);
}

static void stop_running(void)
{
    if (D.t) __atomic_store_n(&D.t->cancel, 1, __ATOMIC_RELAXED);
    D.t = NULL;
}

static void start_now(const char *s, int n)
{
    if (!br.uri) return;
    stop_running();
    if (search_running()) fr_search_cancel();
    if (br.filtering) filter_stop();
    net_prepare(br.uri);
    browser_results_begin();
    br.dupes = 1;
    free(br.search_term);
    br.search_term = NULL;
    task *t = fr_realloc(NULL, sizeof *t);
    *t = (task){.root = fr_strdup(br.uri), .hidden = br.show_hidden};
    D = (run){.t = t, .owner = br.id};
    fr_async_run(work, done, t);
    ui.dirty = 1;
}

void dupes_start(void) { app_defer(start_now, NULL, 0); }

int dupes_running(void) { return D.t && D.owner == br.id; }

void dupes_poll(void)
{
    fr_browser *b = D.t ? browser_by_id(D.owner) : NULL;
    if (D.t && (!b || !b->dupes)) stop_running();
    if (D.t) ui_redraw_in(0.25);
}

void dupes_select_copies(void)
{
    if (!br.dupes) {
        ui_toast("First look for duplicates (fd)");
        return;
    }
    int top = 0;
    for (int v = 0; v < br.view.n; v++) top = browser_item(v)->group > top ? browser_item(v)->group : top;
    char *seen = calloc((size_t)top + 1, 1);
    browser_clear_selection();
    int n = 0;
    for (int v = 0; v < br.view.n; v++) {
        fr_item *it = browser_item(v);
        if (seen[it->group]) browser_set_selected(br.view.v[v], 1), n++;
        seen[it->group] = 1;
    }
    free(seen);
    if (n) ui_toast("Selected %d %s; the first of each group is left", n, n == 1 ? "copy" : "copies");
}

static void leave_now(const char *s, int n)
{
    if (D.owner == br.id) stop_running();
    browser_results_leave();
}

int dupes_header(Rectangle r)
{
    if (!br.dupes) return 0;
    float b = DP(ui.touch ? 48 : 36), x = r.x + DP(4), right = r.x + r.width - DP(4), cy = r.y + (r.height - b) / 2;
    int live = D.owner == br.id, running = live && D.t;
    if (ui_icon_button(ui_id("dupes-close", 0), (Rectangle){x, cy, b, b}, "arrow-left", theme.text)) app_defer(leave_now, NULL, 0);
    x += b + DP(8);
    right -= b;
    if (ui_icon_button(ui_id("more", 0), (Rectangle){right, cy, b, b}, "ellipsis-vertical", theme.text))
        ui_menu_open(app.menu_more, (Rectangle){right, cy, b, b});
    right -= b;
    if (running) {
        if (ui_icon_button(ui_id("dupes-cancel", 0), (Rectangle){right, cy, b, b}, "x", theme.text)) stop_running(), D.rc = -ECANCELED;
    } else {
        if (ui_icon_button(ui_id("dupes-again", 0), (Rectangle){right, cy, b, b}, "refresh-cw", theme.text)) dupes_start();
        if (br.view.n) {
            right -= b;
            if (ui_icon_button(ui_id("dupes-select", 0), (Rectangle){right, cy, b, b}, "list-checks", theme.text)) dupes_select_copies();
        }
    }
    char *folder = fr_uri_name(br.uri), title[400], line[400], wasted[32];
    snprintf(title, sizeof title, "Duplicates in %s", *folder ? folder : "/");
    free(folder);
    float fraction = -1;
    if (running) {
        fr_dupes_status *st = &D.t->st;
        int64_t files = __atomic_load_n(&st->files, __ATOMIC_RELAXED);
        uint64_t total = __atomic_load_n(&st->hash_total, __ATOMIC_RELAXED), got = __atomic_load_n(&st->hash_done, __ATOMIC_RELAXED);
        if (__atomic_load_n(&st->phase, __ATOMIC_RELAXED) == FR_DUPES_LISTING)
            snprintf(line, sizeof line, "Looking: %lld files so far", (long long)files);
        else {
            char a[32], t[32];
            fr_format_size(got, a, sizeof a), fr_format_size(total, t, sizeof t);
            snprintf(line, sizeof line, "Comparing the %lld files' contents: %s of %s", (long long)files, a, t);
            fraction = total ? (float)((double)got / (double)total) : 0;
        }
    } else if (!live) {
        snprintf(line, sizeof line, "%d files", br.view.n);
    } else if (D.rc == -ECANCELED) {
        snprintf(line, sizeof line, "Stopped before the end");
    } else {
        fr_format_size(D.wasted, wasted, sizeof wasted);
        int n = snprintf(line, sizeof line, "%d %s of %d files; %s in the copies beyond the first", D.ngroups,
                         D.ngroups == 1 ? "group" : "groups", D.nfiles, wasted);
        if (D.unreadable && n > 0 && n < (int)sizeof line)
            snprintf(line + n, sizeof line - (size_t)n, "; %lld could not be read", (long long)D.unreadable);
    }
    float w = right - x - DP(8), l1 = ui_line_height(15), l2 = ui_line_height(13), top = r.y + (r.height - l1 - l2) / 2;
    ui_text_box(title, (Rectangle){x, top, w, l1}, 15, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    ui_text_box(line, (Rectangle){x, top + l1, w, l2}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 1);
    Rectangle bar = {r.x, r.y + r.height - DP(3), r.width, DP(2)};
    if (running) ui_progress(bar, fraction >= 0 ? fraction : (float)(ui.now - (long)ui.now), theme.accent);
    else ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
    return 1;
}

#include "app.h"
#include "core/match.h"
#include "core/watch.h"
#include "jobs/async.h"
#include <dirent.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

fr_tabs tabs;
fr_browser *br_cur;

/* the right pane's widget ids: a folder shown in both panes has the same row names twice */
#define RIGHT_PANE_SALT 0x5bd1e995u

static unsigned next_id;

fr_browser *browser_new(const fr_browser *like)
{
    fr_browser *b = fr_realloc(NULL, sizeof *b);
    *b = (fr_browser){.id = ++next_id, .watch = -1};
    if (like) b->sort = like->sort, b->desc = like->desc, b->show_hidden = like->show_hidden, b->layout = like->layout;
    return b;
}

void browser_free(fr_browser *b)
{
    if (!b) return;
    for (int i = 0; i < b->items.n; i++) fr_entry_free(&b->items.v[i].e);
    free(b->items.v);
    free(b->view.v);
    fr_strs_free(&b->back);
    fr_strs_free(&b->forward);
    free(b->uri), free(b->error), free(b->focus_name), free(b->search_term), free(b->shown_uri), free(b->prev_uri);
    fr_watch_remove(b->watch);
    free(b->watch_uri);
    free(b);
}

fr_browser *browser_by_id(unsigned id)
{
    for (int i = 0; i < tabs.n; i++)
        for (int s = 0; s < 2; s++)
            if (tabs.v[i].pane[s] && tabs.v[i].pane[s]->id == id) return tabs.v[i].pane[s];
    return NULL;
}

void browser_enter(fr_browser *b)
{
    int side = 0;
    if (tabs_find(&tabs, b, &side) < 0) side = 0;
    br_cur = b;
    ui.id_salt = side ? RIGHT_PANE_SALT : 0;
}

void tabs_sync(void)
{
    fr_browser *b = tabs_current(&tabs);
    if (b) browser_enter(b);
}

int browser_active(void) { return br_cur == tabs_current(&tabs); }

static void items_free(void)
{
    for (int i = 0; i < br.items.n; i++) fr_entry_free(&br.items.v[i].e);
    br.items.n = 0;
    br.view.n = 0;
    br.nselected = 0;
}

static int64_t dir_mtime(const char *uri)
{
    fr_entry e = {0};
    int64_t t = fr_vfs_stat(uri, &e) ? -1 : e.mtime;
    fr_entry_free(&e);
    return t;
}

static int cmp_items(const void *pa, const void *pb)
{
    const fr_item *a = &br.items.v[*(const int *)pa], *b = &br.items.v[*(const int *)pb];
    if (br.dupes && a->group != b->group) return a->group - b->group;
    /* name search results sorted by name: the closest match first, scored on the name, not the folders above it */
    if (br.results && !br.dupes && !br.search_content && br.search_term && br.sort == SORT_NAME && !strpbrk(br.search_term, "*?")) {
        const char *na = strrchr(a->e.name, '/'), *nb = strrchr(b->e.name, '/');
        int x = fr_fuzzy_score(br.search_term, na ? na + 1 : a->e.name), y = fr_fuzzy_score(br.search_term, nb ? nb + 1 : b->e.name);
        if (x != y) return y - x;
    }
    if (a->e.is_dir != b->e.is_dir) return a->e.is_dir ? -1 : 1;
    int c = 0;
    uint64_t sa = a->e.is_dir ? a->tree_size : a->e.size, sb = b->e.is_dir ? b->tree_size : b->e.size;
    switch (br.sort) {
    case SORT_SIZE: c = sa < sb ? -1 : sa > sb; break;
    case SORT_DATE: c = a->e.mtime < b->e.mtime ? -1 : a->e.mtime > b->e.mtime; break;
    case SORT_KIND: c = (int)a->kind - (int)b->kind; break;
    case SORT_EXT: c = fr_natcmp(a->e.is_dir ? "" : fr_ext(a->e.name), b->e.is_dir ? "" : fr_ext(b->e.name)); break;
    default: break;
    }
    if (!c) c = fr_natcmp(a->e.name, b->e.name);
    if (!c) c = strcmp(a->e.name, b->e.name);
    return br.desc ? -c : c;
}

/* the cursor stays on focus_name, else on kept, else on the name it is on */
static void rebuild(const char *kept)
{
    const char *keep = NULL;
    fr_item *cur = kept ? NULL : browser_item(br.cursor);
    char *keep_name = cur ? fr_strdup(cur->e.name) : NULL;
    keep = br.focus_name ? br.focus_name : kept ? kept : keep_name;
    br.view.n = 0;
    int placeholder = -1;
    for (int i = 0; i < br.items.n; i++) {
        fr_item *it = &br.items.v[i];
        if (it->placeholder) {
            placeholder = i;
            continue;
        }
        if (it->e.hidden && !br.show_hidden) continue;
        if (br.filtering && br.filter.len && !fr_name_matches(br.filter.buf, it->e.name)) continue;
        if (!kind_filter_shows(br.kinds, it->kind)) continue;
        FR_PUSH(br.view, i);
    }
    if (br.view.n > 1) qsort(br.view.v, (size_t)br.view.n, sizeof *br.view.v, cmp_items);
    br.cursor = br.view.n ? 0 : -1;
    for (int i = 0; keep && i < br.view.n; i++)
        if (!strcmp(br.items.v[br.view.v[i]].e.name, keep)) br.cursor = i;
    if (placeholder >= 0) {
        FR_PUSH(br.view, 0);
        memmove(br.view.v + 1, br.view.v, sizeof *br.view.v * (size_t)(br.view.n - 1));
        br.view.v[0] = placeholder;
        br.cursor = 0;
    }
    if (br.anchor < 0 || br.anchor >= br.view.n) br.anchor = br.cursor;
    free(keep_name);
    ui.dirty = 1;
}

static void rebuild_now(const char *s, int n) { rebuild(NULL); }

void browser_rebuild_view(void) { app_defer(rebuild_now, NULL, 0); }

static void push_placeholder(void)
{
    fr_item it = {.kind = KIND_OTHER, .count = -1, .placeholder = 1};
    it.e.name = fr_strdup("");
    FR_PUSH(br.items, it);
}

/* the listing of uri (rc its result) as the items, keeping the selection when uri is the folder shown; takes es */
static void apply(const char *uri, fr_entries *es, int rc)
{
    fr_strs selected = {0};
    int naming = 0;
    for (int i = 0; i < br.items.n; i++) {
        if (br.items.v[i].selected) FR_PUSH(selected, fr_strdup(br.items.v[i].e.name));
        naming |= br.items.v[i].placeholder;
    }
    int same = br.uri && !strcmp(br.uri, uri);
    fr_item *cur = same ? browser_item(br.cursor) : NULL;
    char *kept = cur && !cur->placeholder ? fr_strdup(cur->e.name) : NULL;
    items_free();
    free(br.error);
    br.error = NULL;
    if (rc) br.error = fr_strdup(fr_vfs_error(rc));
    for (int i = 0; i < es->n; i++) {
        fr_item it = {.e = es->v[i], .kind = kind_of(es->v[i].name, es->v[i].is_dir), .count = -1};
        if (same)
            for (int k = 0; k < selected.n; k++)
                if (!strcmp(selected.v[k], it.e.name)) it.selected = 1, br.nselected++;
        FR_PUSH(br.items, it);
    }
    free(es->v);
    *es = (fr_entries){0};
    fr_strs_free(&selected);
    if (same && naming) push_placeholder();
    br.dir_mtime = fr_uri_local(uri) ? dir_mtime(uri) : 0;
    br.checked_at = ui.now;
    br.loads++;
    if (!same) {
        free(br.uri);
        br.uri = fr_strdup(uri);
    }
    if (br.recent_pending && !rc) recent_visit(uri);
    br.recent_pending = 0;
    sizes_apply();
    rebuild(kept);
    free(kept);
}

static unsigned serial; /* of the last listing asked for, by any browser */

/* the browser waiting for that listing: none when another was asked for since, or its tab closed */
static fr_browser *awaiting(unsigned listing)
{
    for (int i = 0; listing && i < tabs.n; i++)
        for (int s = 0; s < 2; s++)
            if (tabs.v[i].pane[s] && tabs.v[i].pane[s]->listing == listing) return tabs.v[i].pane[s];
    return NULL;
}

static void listed(const char *uri, fr_entries *es, int rc, void *ctx)
{
    fr_browser *b = awaiting((unsigned)(uintptr_t)ctx), *was = br_cur;
    if (!b || !b->uri || strcmp(b->uri, uri)) return;
    unsigned salt = ui.id_salt;
    browser_enter(b);
    br.loading = 0;
    br.listing = 0;
    if (rc) net_ask(uri, rc, browser_reload);
    apply(uri, es, rc);
    if (br.focus_name && br.cursor >= 0) view_reveal(br.cursor);
    free(br.focus_name);
    br.focus_name = NULL;
    br_cur = was;
    ui.id_salt = salt;
}

/* A folder on this device is listed at once; a remote one on a thread of its own (listed() takes the answer), the
 * last items staying until then when it is the same folder again. */
static void restat_results(void);

static void load(const char *uri)
{
    if (br.results && br.uri && !strcmp(br.uri, uri)) {
        restat_results();
        return;
    }
    br.results = br.dupes = 0;
    br.listing = 0;
    br.loading = 0;
    if (fr_uri_local(uri) || !fr_vfs_backend(uri)) {
        fr_entries es = {0};
        int rc = fr_vfs_list(uri, &es);
        apply(uri, &es, rc);
        return;
    }
    if (!br.uri || strcmp(br.uri, uri)) {
        items_free();
        free(br.error);
        br.error = NULL;
        free(br.uri);
        br.uri = fr_strdup(uri);
        rebuild_now(NULL, 0);
    }
    br.loading = 1;
    br.listing = ++serial;
    net_prepare(uri);
    fr_async_list(uri, listed, (void *)(uintptr_t)br.listing);
}

/* ---- search results ---- */

static void restat_results(void)
{
    /* taken before the items move: the view's indices point into them until the rebuild */
    fr_item *cur = browser_item(br.cursor);
    char *cursor_name = cur && !cur->placeholder ? fr_strdup(cur->e.name) : NULL;
    int kept = 0;
    br.nselected = 0;
    for (int i = 0; i < br.items.n; i++) {
        fr_item *it = &br.items.v[i];
        char *uri = fr_uri_join(br.uri, it->e.name);
        fr_entry e = {0};
        int gone = it->placeholder || fr_vfs_stat(uri, &e);
        free(uri);
        if (gone) {
            fr_entry_free(&it->e);
            continue;
        }
        free(e.name);
        e.name = it->e.name, e.hidden = it->e.hidden;
        it->e = e;
        it->kind = kind_of(e.name, e.is_dir);
        br.nselected += it->selected;
        br.items.v[kept++] = *it;
    }
    br.items.n = kept;
    if (br.dupes) {
        /* a group down to one file has no duplicates left */
        int top = 0;
        for (int i = 0; i < kept; i++) top = br.items.v[i].group > top ? br.items.v[i].group : top;
        int *members = calloc((size_t)top + 1, sizeof *members);
        for (int i = 0; i < kept; i++) members[br.items.v[i].group]++;
        br.items.n = br.nselected = 0;
        for (int i = 0; i < kept; i++) {
            fr_item *it = &br.items.v[i];
            if (members[it->group] < 2) {
                fr_entry_free(&it->e);
                continue;
            }
            br.nselected += it->selected;
            br.items.v[br.items.n++] = *it;
        }
        free(members);
    }
    sizes_apply();
    br.view.n = 0;
    rebuild(cursor_name);
    free(cursor_name);
}

void browser_results_begin(void)
{
    items_free();
    free(br.error);
    br.error = NULL;
    br.loading = 0;
    br.listing = 0;
    br.results = 1;
    br.dupes = 0;
    br.filtering = 0;
    br.scroll = (fr_scroll){0};
    br.anchor = 0;
    rebuild_now(NULL, 0);
}

void browser_results_add(fr_entries *es)
{
    if (!br.results || !es->n) return;
    for (int i = 0; i < es->n; i++) {
        fr_item it = {.e = es->v[i], .kind = kind_of(es->v[i].name, es->v[i].is_dir), .count = -1};
        FR_PUSH(br.items, it);
    }
    free(es->v);
    *es = (fr_entries){0};
    rebuild_now(NULL, 0);
}

static void load(const char *uri);

void browser_results_leave(void)
{
    if (!br.results) return;
    br.results = br.dupes = 0;
    if (br.uri) load(br.uri);
}

void browser_results_renamed(const char *from, const char *to)
{
    if (!br.results || !br.uri) return;
    for (int i = 0; i < br.items.n; i++) {
        fr_item *it = &br.items.v[i];
        char *uri = fr_uri_join(br.uri, it->e.name);
        if (fr_uri_within(from, uri) && fr_uri_within(br.uri, to)) {
            char *moved = fr_format("%s%s", to, uri + strlen(from));
            const char *base = fr_uri_path(br.uri), *path = fr_uri_path(moved);
            size_t bl = strlen(base);
            if (!strncmp(path, base, bl)) {
                const char *rel = path + bl;
                while (*rel == '/') rel++;
                free(it->e.name);
                it->e.name = fr_strdup(rel);
            }
            free(moved);
        }
        free(uri);
    }
}

static void reload_now(const char *s, int n)
{
    if (br.uri) load(br.uri);
    if (br.loading) return;
    free(br.focus_name);
    br.focus_name = NULL;
}

void browser_reload(void) { app_defer(reload_now, NULL, 0); }

static void open_now(const char *uri, int push_history)
{
    if (br.uri && !strcmp(br.uri, uri)) {
        reload_now(NULL, 0);
        return;
    }
    char *previous = br.uri ? fr_strdup(br.uri) : NULL;
    if (previous) {
        free(br.prev_uri);
        br.prev_uri = fr_strdup(previous);
    }
    if (push_history && previous) {
        FR_PUSH(br.back, fr_strdup(previous));
        fr_strs_free(&br.forward);
    }
    /* going up lands on the folder just left */
    if (!br.focus_name && previous) {
        char *parent = fr_uri_parent(previous);
        if (parent && !strcmp(parent, uri)) br.focus_name = fr_uri_name(previous);
        free(parent);
    }
    br.filtering = 0;
    text_set(&br.filter, "");
    br.scroll = (fr_scroll){0};
    br.anchor = 0;
    br.nselected = 0;
    for (int i = 0; i < br.items.n; i++) br.items.v[i].selected = 0;
    br.recent_pending = 1;
    load(uri);
    if (br.loading) {
        free(previous);
        return;
    }
    if (br.focus_name && br.cursor >= 0) {
        float rh = app_row_height();
        br.scroll.target = br.scroll.offset = br.cursor * rh - DP(120);
        if (br.scroll.target < 0) br.scroll.target = br.scroll.offset = 0;
    }
    free(br.focus_name);
    br.focus_name = NULL;
    free(previous);
}

void browser_open(const char *uri, int push_history) { app_defer(open_now, uri, push_history); }

static void back_now(const char *s, int n)
{
    if (br.results) {
        browser_results_leave();
        return;
    }
    if (!br.back.n) return;
    char *to = br.back.v[--br.back.n];
    FR_PUSH(br.forward, fr_strdup(br.uri));
    open_now(to, 0);
    free(to);
}

void browser_back(void) { app_defer(back_now, NULL, 0); }

static void forward_now(const char *s, int n)
{
    if (!br.forward.n) return;
    char *to = br.forward.v[--br.forward.n];
    FR_PUSH(br.back, fr_strdup(br.uri));
    open_now(to, 0);
    free(to);
}

void browser_forward(void) { app_defer(forward_now, NULL, 0); }

static void up_now(const char *s, int n)
{
    if (br.results) {
        browser_results_leave();
        return;
    }
    char *parent = fr_uri_parent(br.uri);
    if (parent) open_now(parent, 1);
    free(parent);
}

void browser_up(void) { app_defer(up_now, NULL, 0); }

/* What a command acts on: the selection when the focused row is in it (or on touch, where nothing is focused),
 * else the focused row alone, as vim's yy takes the line the cursor is on. */
static int use_selection(void)
{
    fr_item *it = browser_item(br.cursor);
    return ui.touch || !it || it->selected;
}

fr_strs browser_target_uris(void)
{
    if (use_selection()) return browser_selected_uris();
    fr_strs r = {0};
    FR_PUSH(r, fr_uri_join(br.uri, browser_item(br.cursor)->e.name));
    return r;
}

void browser_flash_targets(void)
{
    if (use_selection()) {
        for (int v = 0; v < br.view.n; v++)
            if (br.items.v[br.view.v[v]].selected) br.items.v[br.view.v[v]].flash_at = ui.now;
    } else browser_item(br.cursor)->flash_at = ui.now;
    ui.dirty = 1;
}

static void add_placeholder_now(const char *s, int n)
{
    for (int i = 0; i < br.items.n; i++)
        if (br.items.v[i].placeholder) return;
    push_placeholder();
    rebuild_now(NULL, 0);
    br.scroll.target = 0;
}

void browser_add_placeholder(void) { app_defer(add_placeholder_now, NULL, 0); }

static void drop_placeholder_now(const char *s, int n)
{
    for (int i = 0; i < br.items.n; i++) {
        if (!br.items.v[i].placeholder) continue;
        fr_entry_free(&br.items.v[i].e);
        memmove(br.items.v + i, br.items.v + i + 1, sizeof *br.items.v * (size_t)(br.items.n - i - 1));
        br.items.n--;
        br.view.n = 0;
        br.cursor = -1;
        rebuild_now(NULL, 0);
        return;
    }
}

void browser_drop_placeholder(void) { app_defer(drop_placeholder_now, NULL, 0); }

void browser_name_row(const char *from, const char *to, int is_dir)
{
    char *parent = fr_uri_parent(to), *name = fr_uri_name(to);
    for (int i = 0; parent && br.uri && !strcmp(parent, br.uri) && i < br.items.n; i++) {
        fr_item *it = &br.items.v[i];
        char *uri = from ? fr_uri_join(br.uri, it->e.name) : NULL;
        int match = from ? !strcmp(uri, from) : it->placeholder;
        free(uri);
        if (!match) continue;
        free(it->e.name);
        it->e.name = fr_strdup(name);
        if (!from) it->placeholder = 0, it->e.is_dir = is_dir;
        it->kind = kind_of(name, it->e.is_dir);
        rebuild_now(NULL, 0);
        break;
    }
    free(parent), free(name);
}

void browser_reload_shown(void)
{
    fr_browser *other = tabs_other(&tabs), *was = br_cur;
    unsigned salt = ui.id_salt;
    browser_reload();
    if (other) {
        browser_enter(other);
        browser_reload();
    }
    br_cur = was;
    ui.id_salt = salt;
}

/* the watch follows the folder shown; -1 when it is polled instead */
static int watch_follow(void)
{
    const char *path = br.uri && !br.results ? fr_uri_local(br.uri) : NULL;
    if (path && br.watch_uri && !strcmp(br.watch_uri, br.uri)) return br.watch;
    if (!path && !br.watch_uri) return -1;
    fr_watch_remove(br.watch);
    free(br.watch_uri);
    br.watch_uri = path ? fr_strdup(br.uri) : NULL;
    br.watch = path ? fr_watch_add(path) : -1;
    br.watch_seen = fr_watch_serial(br.watch);
    return br.watch;
}

static void poll_one(void)
{
    for (int i = 0; i < br.items.n; i++)
        if (br.items.v[i].placeholder) return;
    int w = watch_follow();
    /* a server is asked again on a refresh, and after the jobs that change it */
    if (!br.uri || br.results || !fr_uri_local(br.uri)) return;
    if (w >= 0) {
        unsigned s = fr_watch_serial(w);
        if (s == br.watch_seen) return;
        br.watch_seen = s;
        if (fr_watch_gone(w)) {
            fr_watch_remove(w);
            br.watch = -1;
            free(br.watch_uri);
            br.watch_uri = NULL;
        }
    } else if (ui.now - br.checked_at < 1.5) return;
    br.checked_at = ui.now;
    int64_t t = dir_mtime(br.uri);
    /* the folder went away (a disk ejected or unmounted, deleted elsewhere): home, quietly */
    if (t < 0 && br.dir_mtime >= 0) {
        char *home = plat_home_uri();
        browser_open(home, 0);
        free(home);
    } else if (t != br.dir_mtime || w >= 0) browser_reload(); /* inotify also hears of files written, which leave the folder's mtime */
}

/* every tab's, so one in the background is current when it comes back */
void browser_poll(void)
{
    fr_browser *was = br_cur;
    unsigned salt = ui.id_salt;
    for (int i = 0; i < tabs.n; i++)
        for (int s = 0; s < 2; s++)
            if (tabs.v[i].pane[s]) {
                browser_enter(tabs.v[i].pane[s]);
                poll_one();
            }
    br_cur = was;
    ui.id_salt = salt;
}

fr_item *browser_item(int view_index)
{
    return view_index >= 0 && view_index < br.view.n ? &br.items.v[br.view.v[view_index]] : NULL;
}

fr_item *browser_item_of(const char *uri)
{
    for (int i = 0; br.uri && i < br.items.n; i++) {
        char *u = fr_uri_join(br.uri, br.items.v[i].e.name);
        int same = !strcmp(u, uri);
        free(u);
        if (same) return &br.items.v[i];
    }
    return NULL;
}

void browser_set_selected(int item, int on)
{
    fr_item *it = &br.items.v[item];
    if (it->selected == on) return;
    it->selected = on;
    br.nselected += on ? 1 : -1;
    ui.dirty = 1;
}

void browser_select_all(void)
{
    for (int i = 0; i < br.view.n; i++) browser_set_selected(br.view.v[i], 1);
}

void browser_clear_selection(void)
{
    for (int i = 0; i < br.items.n; i++) br.items.v[i].selected = 0;
    br.nselected = 0;
    ui.dirty = 1;
}

fr_strs browser_selected_uris(void)
{
    fr_strs r = {0};
    for (int i = 0; i < br.view.n; i++) {
        fr_item *it = &br.items.v[br.view.v[i]];
        if (it->selected) FR_PUSH(r, fr_uri_join(br.uri, it->e.name));
    }
    return r;
}

/* Counting a folder's items opens it, so only the rows on screen are counted, a few per frame. */
void browser_count_some(int first, int last)
{
    const char *base = fr_uri_local(br.uri);
    if (!base) return;
    int budget = 24;
    for (int v = first; v <= last && budget > 0; v++) {
        fr_item *it = browser_item(v);
        if (!it || !it->e.is_dir || it->count != -1) continue;
        budget--;
        char *path = fr_format("%s%s%s", base, base[strlen(base) - 1] == '/' ? "" : "/", it->e.name);
        DIR *d = opendir(path);
        free(path);
        if (!d) {
            it->count = -2;
            continue;
        }
        int n = 0;
        struct dirent *de;
        while ((de = readdir(d)))
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") && (br.show_hidden || de->d_name[0] != '.')) n++;
        closedir(d);
        it->count = n;
    }
    if (budget <= 0) ui.dirty = 1;
}

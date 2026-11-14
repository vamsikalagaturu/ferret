#include "app.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

fr_browser br;

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
    if (a->e.is_dir != b->e.is_dir) return a->e.is_dir ? -1 : 1;
    int c = 0;
    switch (br.sort) {
    case SORT_SIZE:
        if (!a->e.is_dir) c = a->e.size < b->e.size ? -1 : a->e.size > b->e.size;
        break;
    case SORT_DATE: c = a->e.mtime < b->e.mtime ? -1 : a->e.mtime > b->e.mtime; break;
    case SORT_KIND: c = (int)a->kind - (int)b->kind; break;
    case SORT_EXT: c = fr_natcmp(a->e.is_dir ? "" : fr_ext(a->e.name), b->e.is_dir ? "" : fr_ext(b->e.name)); break;
    default: break;
    }
    if (!c) c = fr_natcmp(a->e.name, b->e.name);
    if (!c) c = strcmp(a->e.name, b->e.name);
    return br.desc ? -c : c;
}

static void rebuild_now(const char *s, int n)
{
    const char *keep = NULL;
    fr_item *cur = browser_item(br.cursor);
    char *keep_name = cur ? fr_strdup(cur->e.name) : NULL;
    keep = br.focus_name ? br.focus_name : keep_name;
    br.view.n = 0;
    int placeholder = -1;
    for (int i = 0; i < br.items.n; i++) {
        fr_item *it = &br.items.v[i];
        if (it->placeholder) {
            placeholder = i;
            continue;
        }
        if (it->e.hidden && !br.show_hidden) continue;
        if (br.filtering && br.filter.len && !fr_contains_nocase(it->e.name, br.filter.buf)) continue;
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

void browser_rebuild_view(void) { app_defer(rebuild_now, NULL, 0); }

static void push_placeholder(void)
{
    fr_item it = {.kind = KIND_OTHER, .count = -1, .placeholder = 1};
    it.e.name = fr_strdup("");
    FR_PUSH(br.items, it);
}

static void load(const char *uri)
{
    fr_strs selected = {0};
    int naming = 0;
    for (int i = 0; i < br.items.n; i++) {
        if (br.items.v[i].selected) FR_PUSH(selected, fr_strdup(br.items.v[i].e.name));
        naming |= br.items.v[i].placeholder;
    }
    int same = br.uri && !strcmp(br.uri, uri);
    items_free();
    free(br.error);
    br.error = NULL;
    fr_entries es = {0};
    int rc = fr_vfs_list(uri, &es);
    if (rc) br.error = fr_strdup(fr_vfs_error(rc));
    for (int i = 0; i < es.n; i++) {
        fr_item it = {.e = es.v[i], .kind = kind_of(es.v[i].name, es.v[i].is_dir), .count = -1};
        if (same)
            for (int k = 0; k < selected.n; k++)
                if (!strcmp(selected.v[k], it.e.name)) it.selected = 1, br.nselected++;
        FR_PUSH(br.items, it);
    }
    free(es.v);
    fr_strs_free(&selected);
    if (same && naming) push_placeholder();
    br.dir_mtime = dir_mtime(uri);
    br.checked_at = ui.now;
    if (!same) {
        free(br.uri);
        br.uri = fr_strdup(uri);
    }
    rebuild_now(NULL, 0);
}

static void reload_now(const char *s, int n)
{
    if (br.uri) load(br.uri);
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
    load(uri);
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

void browser_poll(void)
{
    for (int i = 0; i < br.items.n; i++)
        if (br.items.v[i].placeholder) return;
    if (!br.uri || ui.now - br.checked_at < 1.5) return;
    br.checked_at = ui.now;
    int64_t t = dir_mtime(br.uri);
    /* the folder went away (a disk ejected or unmounted, deleted elsewhere): home, quietly */
    if (t < 0 && br.dir_mtime >= 0) {
        char *home = plat_home_uri();
        browser_open(home, 0);
        free(home);
    } else if (t != br.dir_mtime) browser_reload();
}

fr_item *browser_item(int view_index)
{
    return view_index >= 0 && view_index < br.view.n ? &br.items.v[br.view.v[view_index]] : NULL;
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

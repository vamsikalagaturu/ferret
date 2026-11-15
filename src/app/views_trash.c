/* trash:/// as a list of what was deleted: the name it had, where it was, when it went. Restoring and deleting
 * for good act on the selection, or the cursor row. */
#include "views.h"
#include "vfs/trash.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* each item's origin, read once per listing of the trash */
static struct {
    struct origin {
        char *key, *path;
        int64_t deleted;
    } *v;
    int n, cap;
    int64_t listed_mtime;
    int listed_items;
} origins;

static void origins_clear(void)
{
    for (int i = 0; i < origins.n; i++) free(origins.v[i].key), free(origins.v[i].path);
    origins.n = 0;
}

static const struct origin *origin_of(const fr_item *it)
{
    if (origins.listed_mtime != br.dir_mtime || origins.listed_items != br.items.n) {
        origins_clear();
        origins.listed_mtime = br.dir_mtime;
        origins.listed_items = br.items.n;
    }
    for (int i = 0; i < origins.n; i++)
        if (!strcmp(origins.v[i].key, it->e.name)) return &origins.v[i];
    char *uri = fr_uri_join(br.uri, it->e.name);
    struct origin o = {fr_strdup(it->e.name), NULL, -1};
    if (fr_trash_origin(uri, &o.path, &o.deleted)) o.path = NULL;
    free(uri);
    FR_PUSH(origins, o);
    return &origins.v[origins.n - 1];
}

/* "~/Documents" for a folder below home */
static void shown_folder(const char *path, char *out, size_t len)
{
    const char *slash = strrchr(path, '/'), *home = getenv("HOME");
    int dir_len = slash == path ? 1 : (int)(slash - path);
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(path, home, hl) && (path[hl] == '/') && (int)hl <= dir_len)
        snprintf(out, len, "~%.*s", dir_len - (int)hl, path + hl);
    else snprintf(out, len, "%.*s", dir_len, path);
}

static unsigned menu_id(void) { return ui_id("menu-trash", 0); }

static void restore_now(const char *s, int n)
{
    fr_strs uris = browser_target_uris();
    int done = 0, failed = 0, first_err = 0;
    char *first_name = NULL, *landed = NULL;
    for (int i = 0; i < uris.n; i++) {
        char *to = NULL;
        int rc = fr_trash_restore(uris.v[i], &to);
        if (!rc) {
            done++;
            free(landed);
            landed = to;
        } else if (!failed++) {
            first_err = rc;
            first_name = fr_uri_name(uris.v[i]);
        }
    }
    if (failed) ui_toast("Could not restore %s: %s", first_name, fr_vfs_error(first_err));
    else if (done == 1) {
        const char *p = fr_uri_local(landed);
        ui_toast("Restored to %s", p ? p : landed);
    } else if (done) ui_toast("Restored %d items", done);
    free(first_name), free(landed);
    fr_strs_free(&uris);
    browser_clear_selection();
    browser_reload();
}

void trash_restore(void) { app_defer(restore_now, NULL, 0); }

void trash_empty(void)
{
    fr_entries es = {0};
    fr_vfs_list(FR_TRASH_URI, &es);
    fr_strs uris = {0};
    for (int i = 0; i < es.n; i++) FR_PUSH(uris, fr_uri_join(FR_TRASH_URI, es.v[i].name));
    fr_entries_free(&es);
    if (!uris.n) {
        ui_toast("The trash is empty");
        return;
    }
    char msg[160];
    snprintf(msg, sizeof msg, "%d %s will be deleted for good. This cannot be undone.", uris.n, uris.n == 1 ? "item" : "items");
    ops_delete_uris(&uris, "Empty the trash?", msg);
}

void trash_draw_menus(void)
{
    if (!ui_menu_begin(menu_id())) return;
    if (ui_menu_item("Restore", "rotate-ccw", -1)) trash_restore();
    if (ui_menu_item("Copy", "copy", -1)) ops_copy(0);
    ui_menu_separator();
    if (ui_menu_item("Delete permanently", "trash-2", -1)) ops_delete(1);
    if (ui_menu_item("Empty trash", "trash", -1)) trash_empty();
    ui_menu_separator();
    if (ui_menu_item("Properties", "info", -1)) ops_properties();
    ui_menu_end();
}

typedef struct {
    float name, name_w, from, from_w, when, when_w, size, size_w;
} cells;

/* narrow lists drop the size, then the date */
static cells cells_for(Rectangle row)
{
    float pad = DP(12), gap = DP(16), x = row.x + row.width - pad;
    cells c = {0};
    c.size_w = row.width >= DP(640) ? DP(80) : 0;
    c.when_w = row.width >= DP(480) ? DP(120) : 0;
    if (c.size_w) c.size = x - c.size_w, x = c.size - gap;
    if (c.when_w) c.when = x - c.when_w, x = c.when - gap;
    c.name = row.x + pad + DP(40);
    float rest = x - c.name;
    c.name_w = roundf(rest * 0.45f);
    c.from = c.name + c.name_w + gap;
    c.from_w = rest - c.name_w - gap;
    return c;
}

static void draw_header(Rectangle h)
{
    cells c = cells_for(h);
    struct {
        const char *label;
        int sort; /* -1: not sortable */
        float x, w;
    } cols[] = {{"Name", SORT_NAME, c.name, c.name_w}, {"Original location", -1, c.from, c.from_w},
                {"Deleted", SORT_DATE, c.when, c.when_w}, {"Size", SORT_SIZE, c.size, c.size_w}};
    for (int i = 0; i < 4; i++) {
        if (cols[i].w <= 0) continue;
        Rectangle hit = {cols[i].x - DP(8), h.y, cols[i].w + DP(16), h.height};
        int on = cols[i].sort >= 0 && br.sort == (fr_sort)cols[i].sort, state = 0;
        if (cols[i].sort >= 0 && ui_clickable(ui_id("trash-sort", i), hit, &state)) {
            if (on) br.desc = !br.desc;
            else br.sort = (fr_sort)cols[i].sort, br.desc = cols[i].sort == SORT_DATE;
            browser_rebuild_view();
        }
        if (state) ui_rect(hit, state == 2 ? theme.pressed : theme.hover);
        float lw = ui_text_width(cols[i].label, 13, FONT_SEMIBOLD);
        ui_text_box(cols[i].label, (Rectangle){cols[i].x, h.y, cols[i].w, h.height}, 13, FONT_SEMIBOLD, on ? theme.text : theme.text_dim, ALIGN_LEFT, 0);
        if (on) ui_icon(br.desc ? "chevron-down" : "chevron-up", (Rectangle){cols[i].x + lw + DP(2), h.y, DP(16), h.height}, 14, theme.text_dim);
    }
    ui_hline(h.x, h.y + h.height - DP(1), h.width, theme.border);
}

static void draw_row(int v, Rectangle row, Rectangle area)
{
    fr_item *it = browser_item(v);
    int state;
    Rectangle hit = GetCollisionRec(row, area);
    int clicked = ui_clickable(ui_id(it->e.name, (int)ui_id("row", 0)), hit, &state);
    int right = ui_hover(hit) && ui.rpressed;
    if (it->selected) ui_rect(row, theme.selection);
    else if (state) ui_rect(row, state == 2 ? theme.pressed : theme.hover);
    view_flash(it, row);
    if (br.keyboard && v == br.cursor && !it->selected)
        ui_round_line((Rectangle){row.x + DP(2), row.y + DP(1), row.width - DP(4), row.height - DP(2)}, 4, ui_alpha(theme.accent, 0.5f));
    const struct origin *o = origin_of(it);
    const char *slash = o->path ? strrchr(o->path, '/') : NULL;
    const char *name = slash && slash[1] ? slash + 1 : it->e.name;
    fr_kind k = kind_of(name, it->e.is_dir);
    cells c = cells_for(row);
    ui_icon(kind_icon(k), (Rectangle){row.x + DP(12), row.y, DP(32), row.height}, 20, kind_color(k));
    ui_text_box(name, (Rectangle){c.name, row.y, c.name_w, row.height}, 15, FONT_REGULAR, theme.text, ALIGN_LEFT, 1);
    char from[1024], when[32], size[32] = "";
    if (o->path) shown_folder(o->path, from, sizeof from);
    else snprintf(from, sizeof from, "Unknown");
    ui_text_box(from, (Rectangle){c.from, row.y, c.from_w, row.height}, 14, FONT_REGULAR, o->path ? theme.text_dim : theme.text_faint, ALIGN_LEFT, 1);
    if (c.when_w) {
        fr_format_time(it->e.mtime, (int64_t)time(NULL), when, sizeof when);
        ui_text_box(when, (Rectangle){c.when, row.y, c.when_w, row.height}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    }
    if (c.size_w) {
        if (!it->e.is_dir) fr_format_size(it->e.size, size, sizeof size);
        ui_text_box(size, (Rectangle){c.size, row.y, c.size_w, row.height}, 14, FONT_REGULAR, theme.text_dim, ALIGN_RIGHT, 0);
    }
    ui_hline(c.name, row.y + row.height - DP(1), row.x + row.width - c.name, ui_alpha(theme.border, 0.6f));
    if (clicked) view_row_pressed(v, 0);
    else if (right) {
        if (!it->selected) browser_clear_selection(), browser_set_selected(br.view.v[v], 1);
        br.cursor = br.anchor = v;
        ui_menu_open(menu_id(), (Rectangle){ui.mouse.x, ui.mouse.y, 0, 0});
    }
}

static void draw_bar(Rectangle r)
{
    ui_rect(r, theme.surface);
    ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
    float pad = DP(12), bh = DP(32), by = r.y + (r.height - bh) / 2, x = r.x + r.width - pad;
    int targets = br.nselected || browser_item(br.cursor);
    const char *labels[] = {"Empty trash", "Delete", "Restore"};
    for (int i = 0; i < 3; i++) {
        if (i && !targets) break;
        float bw = ui_button_width(labels[i]);
        x -= bw;
        if (ui_button(ui_id("trash-bar", i), (Rectangle){x, by, bw, bh}, labels[i], i == 2)) {
            if (i == 0) trash_empty();
            else if (i == 1) ops_delete(1);
            else trash_restore();
        }
        x -= DP(8);
    }
    char note[96];
    snprintf(note, sizeof note, "%d %s from every trash folder", br.view.n, br.view.n == 1 ? "item" : "items");
    ui_text_box(note, (Rectangle){r.x + pad, r.y, x - r.x - 2 * pad, r.height}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
}

void trash_draw(Rectangle r)
{
    if (br.error || (!br.view.n && br.filtering && br.filter.len)) {
        view_draw_folder_state(r);
        return;
    }
    if (!br.items.n) {
        view_draw_empty(r, "trash-2", "The trash is empty", NULL, 0);
        return;
    }
    float bar = DP(48), hh = DP(30);
    draw_bar((Rectangle){r.x, r.y, r.width, bar});
    draw_header((Rectangle){r.x, r.y + bar, r.width, hh});
    Rectangle list = {r.x, r.y + bar + hh, r.width, r.height - bar - hh};
    float rh = app_row_height();
    ui_scroll(ui_id("list", 0), &br.scroll, list, br.view.n * rh + DP(24));
    int first = (int)(br.scroll.offset / rh), last = (int)((br.scroll.offset + list.height) / rh);
    if (last >= br.view.n) last = br.view.n - 1;
    ui_clip_begin(list);
    for (int v = first; v <= last; v++) draw_row(v, (Rectangle){list.x, roundf(list.y + v * rh - br.scroll.offset), list.width, rh}, list);
    ui_clip_end();
    Rectangle below = {list.x, list.y + (last + 1) * rh - br.scroll.offset, list.width, 0};
    below.height = list.y + list.height - below.y;
    if (below.height > 0 && !ui.touch && ui_hover(below) && ui.pressed) browser_clear_selection();
}

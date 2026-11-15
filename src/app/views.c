#include "views.h"
#include "vfs/trash.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The layout in effect and the grid's columns depend on the width view_draw last had (br.shown_w). A split view's
 * panes show columns as details: Miller columns want the width, and their parent and preview panes are one each. */
static fr_layout layout_in_effect(void)
{
    if (br.uri && fr_trash_is_root(br.uri)) return LAYOUT_DETAILS;
    if (br.layout == LAYOUT_COLUMNS && (ui.touch || app_split() || br.shown_w < DP(720))) return LAYOUT_DETAILS;
    return br.layout;
}

fr_layout view_layout(void) { return layout_in_effect(); }

void view_set_layout(fr_layout layout)
{
    br.layout = layout;
    br.reveal_pending = 1;
    ui.dirty = 1;
}

#define FLASH_S 0.6

void view_flash(const fr_item *it, Rectangle r)
{
    double t = ui.now - it->flash_at;
    if (!it->flash_at || t >= FLASH_S) return;
    ui_rect(r, ui_alpha(theme.accent, 0.4f * (float)(1 - t / FLASH_S)));
    ui_redraw_in(1.0 / 60);
}

int view_grid_columns(void) { return layout_in_effect() == LAYOUT_GRID ? grid_columns_for(br.shown_w) : 1; }

void view_reveal(int v)
{
    if (layout_in_effect() == LAYOUT_GRID) {
        grid_reveal(v, br.shown_w);
        return;
    }
    float rh = app_row_height();
    ui_scroll_to(&br.scroll, v * rh, (v + 1) * rh);
}

void view_row_pressed(int v, int right_click)
{
    fr_item *it = browser_item(v);
    if (!it) return;
    int idx = br.view.v[v];
    br.keyboard = 0;
    if (ui.touch) {
        if (right_click) browser_set_selected(idx, !it->selected);
        else if (br.nselected) browser_set_selected(idx, !it->selected);
        else ops_open_item(it);
        br.cursor = v;
        return;
    }
    if (right_click) {
        if (!it->selected) browser_clear_selection(), browser_set_selected(idx, 1);
        br.cursor = br.anchor = v;
        ui_menu_open(app.menu_row, (Rectangle){ui.mouse.x, ui.mouse.y, 0, 0});
        return;
    }
    int mods = 0;
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) mods |= MOD_CTRL;
    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) mods |= MOD_SHIFT;
    if (mods & MOD_CTRL) {
        browser_set_selected(idx, !it->selected);
        br.cursor = br.anchor = v;
    } else if (mods & MOD_SHIFT) {
        browser_clear_selection();
        int from = br.anchor >= 0 && br.anchor < br.view.n ? br.anchor : v;
        int a = from < v ? from : v, b = from < v ? v : from;
        for (int i = a; i <= b; i++) browser_set_selected(br.view.v[i], 1);
        br.cursor = v;
    } else if (ui.clicks == 2 && br.cursor == v) {
        browser_clear_selection();
        ops_open_item(it);
    } else {
        browser_clear_selection();
        browser_set_selected(idx, 1);
        br.cursor = br.anchor = v;
    }
}

void view_open_in_tab(int v)
{
    fr_item *it = browser_item(v);
    if (!it || it->placeholder || !br.uri) return;
    char *uri = fr_uri_join(br.uri, it->e.name);
    if (it->e.is_dir) tab_new(uri, 0);
    else if (fr_archive_browsable(it->e.name)) {
        char *root = fr_archive_root(uri);
        tab_new(root, 0);
        free(root);
    }
    free(uri);
}

fr_item *view_item_at(Vector2 p)
{
    for (int v = 0; v < br.view.n; v++) {
        fr_item *it = browser_item(v);
        Rectangle r;
        if ((ui_clickable_rect(ui_id(it->e.name, (int)ui_id("row", 0)), &r) || ui_clickable_rect(ui_id(it->e.name, (int)ui_id("tile", 0)), &r)) &&
            CheckCollisionPointRec(p, r))
            return it;
    }
    return NULL;
}

void view_background_input(Rectangle r)
{
    if (ui.touch || !ui_hover(r)) return;
    if (ui.pressed && !(IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_LEFT_SHIFT))) browser_clear_selection();
    if (ui.rpressed) ui_menu_open(app.menu_bg, (Rectangle){ui.mouse.x, ui.mouse.y, 0, 0});
}

/* ---- names on up to two lines ---- */

static int is_break(char c) { return c == ' ' || c == '-' || c == '_'; }

void view_draw_name(const char *s, Rectangle r, float size_dp, Color c)
{
    float lh = ui_line_height(size_dp);
    int n = (int)strlen(s);
    int stops[512], nstops = 0;
    if (ui_text_width_n(s, n, size_dp, FONT_REGULAR) <= r.width || n >= (int)(sizeof stops / sizeof *stops)) {
        ui_text_box(s, (Rectangle){r.x, r.y, r.width, lh}, size_dp, FONT_REGULAR, c, ALIGN_CENTER, 1);
        return;
    }
    for (int i = 0; i < n; i = fr_utf8_next(s, n, i)) stops[nstops++] = i;
    stops[nstops++] = n;
    int lo = 0, hi = nstops - 1; /* stops[lo] fits, stops[hi] does not */
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (ui_text_width_n(s, stops[mid], size_dp, FONT_REGULAR) <= r.width) lo = mid;
        else hi = mid;
    }
    int k = stops[lo];
    for (int i = k; i > k / 2; i--)
        if (is_break(s[i - 1])) {
            k = i;
            break;
        }
    if (k <= 0) k = stops[1];
    char line[512];
    int len = k;
    while (len > 0 && s[len - 1] == ' ') len--;
    memcpy(line, s, (size_t)len);
    line[len] = 0;
    ui_text_box(line, (Rectangle){r.x, r.y, r.width, lh}, size_dp, FONT_REGULAR, c, ALIGN_CENTER, 0);
    const char *rest = s + k;
    while (*rest == ' ') rest++;
    ui_text_box(rest, (Rectangle){r.x, r.y + lh, r.width, lh}, size_dp, FONT_REGULAR, c, ALIGN_CENTER, 1);
}

/* ---- details ---- */

typedef struct {
    float name, name_w, size, size_w, date, date_w, kind, kind_w;
} cells;

/* Header and rows share these; narrow lists drop Kind, then Modified. */
static cells cells_for(Rectangle row)
{
    float pad = DP(12), gap = DP(16), x = row.x + row.width - pad;
    cells c = {0};
    c.kind_w = row.width >= DP(640) ? DP(110) : 0;
    c.date_w = row.width >= DP(480) ? DP(120) : 0;
    c.size_w = DP(80);
    if (c.kind_w) c.kind = x - c.kind_w, x = c.kind - gap;
    if (c.date_w) c.date = x - c.date_w, x = c.date - gap;
    c.size = x - c.size_w, x = c.size - gap;
    c.name = row.x + pad + DP(32) + DP(8);
    c.name_w = x - c.name;
    return c;
}

static void draw_header(Rectangle h)
{
    cells c = cells_for(h);
    float gap = DP(16);
    struct {
        const char *label;
        fr_sort sort;
        float x, w;
        int right;
    } cols[] = {{"Name", SORT_NAME, c.name, c.name_w, 0},
                {"Size", SORT_SIZE, c.size, c.size_w, 1},
                {"Modified", SORT_DATE, c.date, c.date_w, 0},
                {"Kind", SORT_KIND, c.kind, c.kind_w, 0}};
    for (int i = 0; i < 4; i++) {
        if (cols[i].w <= 0) continue;
        Rectangle hit = {cols[i].x - gap / 2, h.y, cols[i].w + gap, h.height};
        if (i == 0) hit.width += hit.x - h.x, hit.x = h.x;
        int state;
        if (ui_clickable(ui_id("sort", i), hit, &state)) {
            if (br.sort == cols[i].sort) br.desc = !br.desc;
            else br.sort = cols[i].sort, br.desc = 0;
            browser_rebuild_view();
        }
        if (state) ui_rect(hit, state == 2 ? theme.pressed : theme.hover);
        int on = br.sort == cols[i].sort;
        float lw = ui_text_width(cols[i].label, 13, FONT_SEMIBOLD), iw = DP(16);
        float lx = cols[i].right ? cols[i].x + cols[i].w - lw : cols[i].x;
        ui_text_box(cols[i].label, (Rectangle){lx, h.y, lw, h.height}, 13, FONT_SEMIBOLD, on ? theme.text : theme.text_dim, ALIGN_LEFT, 0);
        if (on) {
            Rectangle ib = {cols[i].right ? lx - iw - DP(2) : lx + lw + DP(2), h.y, iw, h.height};
            ui_icon(br.desc ? "chevron-down" : "chevron-up", ib, 14, theme.text_dim);
        }
    }
    ui_hline(h.x, h.y + h.height - DP(1), h.width, theme.border);
}

static void draw_row(int v, Rectangle row, Rectangle area)
{
    fr_item *it = browser_item(v);
    int compact = !ui.touch, state;
    Rectangle hit = GetCollisionRec(row, area);
    /* by name: a reload between the press and the release may put another item at v */
    int clicked = ui_clickable(ui_id(it->e.name, (int)ui_id("row", 0)), hit, &state);
    int right = ui_hover(hit) && ui.rpressed;
    if (it->selected) ui_rect(row, theme.selection);
    else if (state) ui_rect(row, state == 2 ? theme.pressed : theme.hover);
    else if (br.dupes && !(it->group & 1)) ui_rect(row, theme.surface); /* every other group tinted, to tell them apart */
    view_flash(it, row);
    if (!ui.touch && br.keyboard && v == br.cursor && !it->selected)
        ui_round_line((Rectangle){row.x + DP(2), row.y + DP(1), row.width - DP(4), row.height - DP(2)}, 4, ui_alpha(theme.accent, 0.5f));

    float pad = DP(ui.touch ? 16 : 12), icon_w = DP(compact ? 32 : 40);
    Rectangle icon = {row.x + pad, row.y, icon_w, row.height};
    Color kc = kind_color(it->kind);
    float side = fminf(icon_w, row.height - DP(8));
    Rectangle square = {icon.x + (icon_w - side) / 2, row.y + (row.height - side) / 2, side, side};
    if (it->selected && ui.touch) ui_icon("square-check", icon, 24, theme.accent);
    else if (thumb_draw(it, square) <= 0) ui_icon(kind_icon(it->kind), icon, compact ? 20 : 24, kc);
    if (it->e.is_link) ui_icon("link", (Rectangle){icon.x + icon_w - DP(14), row.y + row.height / 2, DP(12), DP(12)}, 10, theme.text_faint);

    char size[32] = "", date[32];
    if (it->e.is_dir) {
        uint64_t sofar = 0;
        int measuring = sizes_progress(it, &sofar);
        if (measuring == 1) {
            fr_format_size(sofar, size, sizeof size - 4);
            strcat(size, "…");
        } else if (measuring == 2) snprintf(size, sizeof size, "…");
        else if (it->sized) fr_format_size(it->tree_size, size, sizeof size);
        else if (it->count >= 0) snprintf(size, sizeof size, it->count == 1 ? "1 item" : "%d items", it->count);
    } else fr_format_size(it->e.size, size, sizeof size);
    fr_format_time(it->e.mtime, (int64_t)time(NULL), date, sizeof date);
    if (it->placeholder) size[0] = date[0] = 0;
    float tx = icon.x + icon_w + DP(compact ? 8 : 12), tw = row.x + row.width - pad - tx;
    Color name_c = it->e.hidden ? theme.text_dim : theme.text;
    int editing;
    if (compact) {
        cells c = cells_for(row);
        editing = keys_insert_field(v, (Rectangle){c.name - DP(12), row.y + DP(3), c.name_w + DP(12), row.height - DP(6)});
        if (!editing) ui_text_box(it->e.name, (Rectangle){c.name, row.y, c.name_w, row.height}, 15, FONT_REGULAR, name_c, ALIGN_LEFT, 1);
        ui_text_box(size, (Rectangle){c.size, row.y, c.size_w, row.height}, 14, FONT_REGULAR, theme.text_dim, ALIGN_RIGHT, 0);
        if (c.date_w) ui_text_box(date, (Rectangle){c.date, row.y, c.date_w, row.height}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
        if (c.kind_w) ui_text_box(kind_label(it->kind), (Rectangle){c.kind, row.y, c.kind_w, row.height}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    } else {
        float line1 = row.y + DP(11), line2 = row.y + DP(35);
        editing = keys_insert_field(v, (Rectangle){tx - DP(12), line1 - DP(8), tw + DP(12), DP(36)});
        if (!editing) ui_text_box(it->e.name, (Rectangle){tx, line1, tw, DP(20)}, 16, FONT_REGULAR, name_c, ALIGN_LEFT, 1);
        float sw = ui_text_width(size, 13, FONT_REGULAR);
        ui_text_box(date, (Rectangle){tx, line2, tw - sw - DP(12), DP(18)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
        ui_text_box(size, (Rectangle){tx + tw - sw, line2, sw, DP(18)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_RIGHT, 0);
    }
    ui_hline(tx, row.y + row.height - DP(1), row.x + row.width - tx, ui_alpha(theme.border, 0.6f));
    if (editing && ui_hover(hit)) return;
    if (clicked) view_row_pressed(v, 0);
    else if (right) view_row_pressed(v, 1);
    else if (ui.mpressed && ui_hover(hit)) view_open_in_tab(v);
}

void view_draw_empty(Rectangle r, const char *icon, const char *title, const char *detail, int retry)
{
    float cy = r.y + r.height * 0.3f;
    ui_icon(icon, (Rectangle){r.x, cy, r.width, DP(56)}, 48, theme.text_faint);
    ui_text_box(title, (Rectangle){r.x + DP(24), cy + DP(64), r.width - DP(48), DP(24)}, 17, FONT_SEMIBOLD, theme.text_dim, ALIGN_CENTER, 0);
    if (detail) ui_text_box(detail, (Rectangle){r.x + DP(24), cy + DP(92), r.width - DP(48), DP(20)}, 14, FONT_REGULAR, theme.text_faint, ALIGN_CENTER, 0);
    if (retry) {
        float bw = ui_button_width("Try again");
        if (ui_button(ui_id("retry", 0), (Rectangle){r.x + (r.width - bw) / 2, cy + DP(128), bw, DP(40)}, "Try again", 1)) browser_reload();
    }
}

int view_draw_folder_state(Rectangle r)
{
    if (br.loading && !br.items.n) view_draw_empty(r, "loader-circle", "Loading...", NULL, 0);
    else if (br.error) view_draw_empty(r, "triangle-alert", "This folder cannot be shown", br.error, 1);
    else if (!br.view.n && br.filtering && br.filter.len) view_draw_empty(r, "search", "Nothing matches", NULL, 0);
    else if (!br.view.n && br.kinds && br.items.n) {
        char detail[64];
        snprintf(detail, sizeof detail, "Showing only %s", kind_filter_label(br.kinds));
        view_draw_empty(r, "shapes", "Nothing of this kind here", detail, 0);
    } else if (!br.view.n && br.dupes) view_draw_empty(r, "file-stack", dupes_running() ? "Looking for duplicates..." : "No duplicates found", NULL, 0);
    else if (!br.view.n && br.results) view_draw_empty(r, "search", search_running() ? "Searching..." : "Nothing found", NULL, 0);
    else if (!br.view.n) view_draw_empty(r, "folder-open", "Empty folder", br.items.n ? "Only hidden items are here" : NULL, 0);
    else return 0;
    return 1;
}

void view_list_draw(Rectangle r)
{
    float rh = app_row_height(), content = br.view.n * rh + DP(ui.touch ? 96 : 24);
    ui_scroll(ui_id("list", 0), &br.scroll, r, content);
    int first = (int)(br.scroll.offset / rh), last = (int)((br.scroll.offset + r.height) / rh);
    if (last >= br.view.n) last = br.view.n - 1;
    browser_count_some(first, last);
    ui_clip_begin(r);
    for (int v = first; v <= last; v++) {
        Rectangle row = {r.x, roundf(r.y + v * rh - br.scroll.offset), r.width, rh};
        draw_row(v, row, r);
    }
    ui_clip_end();
    Rectangle below = {r.x, r.y + (last + 1) * rh - br.scroll.offset, r.width, 0};
    below.height = r.y + r.height - below.y;
    if (below.height > 0) view_background_input(below);
}

static void draw_details(Rectangle r)
{
    if (!ui.touch) {
        float hh = DP(30);
        draw_header((Rectangle){r.x, r.y, r.width, hh});
        r.y += hh, r.height -= hh;
    }
    view_list_draw(r);
}

void view_draw(Rectangle r)
{
    ui_rect(r, theme.bg);
    br.shown_w = r.width;
    if (!br.uri || !br.shown_uri || strcmp(br.shown_uri, br.uri)) {
        free(br.shown_uri);
        br.shown_uri = br.uri ? fr_strdup(br.uri) : NULL;
        br.reveal_pending = 1;
    }
    if (br.reveal_pending && br.cursor >= 0) {
        view_reveal(br.cursor);
        br.scroll.offset = br.scroll.target;
    }
    br.reveal_pending = 0;
    fr_layout layout = layout_in_effect();
    if (br.uri && fr_trash_is_root(br.uri)) trash_draw(r);
    else if (layout == LAYOUT_COLUMNS) columns_draw(r);
    else if (view_draw_folder_state(r)) return;
    else if (layout == LAYOUT_GRID) grid_draw(r);
    else draw_details(r);
}

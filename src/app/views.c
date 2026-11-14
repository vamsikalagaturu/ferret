#include "app.h"
#include <math.h>
#include <stdio.h>
#include <time.h>

void view_set_layout(fr_layout layout)
{
    app.layout = layout;
    ui.dirty = 1;
}

void view_reveal(int v)
{
    float rh = app_row_height();
    ui_scroll_to(&br.scroll, v * rh, (v + 1) * rh);
}

static void row_pressed(int v, int right_click)
{
    fr_item *it = browser_item(v);
    if (!it) return;
    int idx = br.view.v[v];
    br.keyboard = 0;
    if (ui.touch) {
        if (right_click) {
            browser_set_selected(idx, !it->selected);
            plat_haptic();
        } else if (br.nselected) browser_set_selected(idx, !it->selected);
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
        int a = br.anchor < v ? br.anchor : v, b = br.anchor < v ? v : br.anchor;
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

static void draw_row(int v, Rectangle row, int compact)
{
    fr_item *it = browser_item(v);
    int state;
    unsigned id = ui_id("row", v);
    int clicked = ui_clickable(id, row, &state);
    int right = ui_hover(row) && ui.rpressed;
    if (it->selected) ui_rect(row, theme.selection);
    else if (state) ui_rect(row, state == 2 ? theme.pressed : theme.hover);
    if (!ui.touch && br.keyboard && v == br.cursor && !it->selected)
        ui_round_line((Rectangle){row.x + DP(2), row.y + DP(1), row.width - DP(4), row.height - DP(2)}, 4, ui_alpha(theme.accent, 0.5f));

    float pad = DP(ui.touch ? 16 : 12), icon_w = DP(compact ? 28 : 40);
    Rectangle icon = {row.x + pad, row.y, icon_w, row.height};
    Color kc = kind_color(it->kind);
    if (it->selected && ui.touch) ui_icon("square-check", icon, 24, theme.accent);
    else ui_icon(kind_icon(it->kind), icon, compact ? 18 : 24, kc);
    if (it->e.is_link) ui_icon("link", (Rectangle){icon.x + icon_w - DP(14), row.y + row.height / 2, DP(12), DP(12)}, 10, theme.text_faint);

    char size[32] = "", date[32];
    if (it->e.is_dir) {
        if (it->count >= 0) snprintf(size, sizeof size, it->count == 1 ? "1 item" : "%d items", it->count);
    } else fr_format_size(it->e.size, size, sizeof size);
    fr_format_time(it->e.mtime, (int64_t)time(NULL), date, sizeof date);
    float tx = icon.x + icon_w + DP(compact ? 8 : 12), tw = row.x + row.width - pad - tx;
    Color name_c = it->e.hidden ? theme.text_dim : theme.text;
    if (compact) {
        float date_w = row.width > DP(560) ? DP(120) : 0, size_w = DP(90);
        float name_w = tw - size_w - date_w - DP(8);
        ui_text_box(it->e.name, (Rectangle){tx, row.y, name_w, row.height}, 14, FONT_REGULAR, name_c, ALIGN_LEFT, 1);
        ui_text_box(size, (Rectangle){tx + name_w, row.y, size_w, row.height}, 13, FONT_REGULAR, theme.text_dim, ALIGN_RIGHT, 0);
        if (date_w) ui_text_box(date, (Rectangle){tx + name_w + size_w, row.y, date_w, row.height}, 13, FONT_REGULAR, theme.text_dim, ALIGN_RIGHT, 0);
    } else {
        float line1 = row.y + DP(11), line2 = row.y + DP(35);
        ui_text_box(it->e.name, (Rectangle){tx, line1, tw, DP(20)}, 16, FONT_REGULAR, name_c, ALIGN_LEFT, 1);
        float sw = ui_text_width(size, 13, FONT_REGULAR);
        ui_text_box(date, (Rectangle){tx, line2, tw - sw - DP(12), DP(18)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
        ui_text_box(size, (Rectangle){tx + tw - sw, line2, sw, DP(18)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_RIGHT, 0);
    }
    ui_hline(tx, row.y + row.height - DP(1), row.x + row.width - tx, ui_alpha(theme.border, 0.6f));
    if (clicked) row_pressed(v, 0);
    else if (right) row_pressed(v, 1);
}

static void draw_empty(Rectangle r, const char *icon, const char *title, const char *detail, int retry)
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

static void draw_details(Rectangle r)
{
    int compact = !ui.touch;
    float rh = app_row_height(), content = br.view.n * rh + DP(ui.touch ? 96 : 24);
    ui_scroll(ui_id("list", 0), &br.scroll, r, content);
    int first = (int)(br.scroll.offset / rh), last = (int)((br.scroll.offset + r.height) / rh);
    if (last >= br.view.n) last = br.view.n - 1;
    browser_count_some(first, last);
    ui_clip_begin(r);
    for (int v = first; v <= last; v++) {
        Rectangle row = {r.x, roundf(r.y + v * rh - br.scroll.offset), r.width, rh};
        draw_row(v, row, compact);
    }
    ui_clip_end();
    Rectangle below = {r.x, r.y + (last + 1) * rh - br.scroll.offset, r.width, 0};
    below.height = r.y + r.height - below.y;
    if (below.height > 0 && ui_hover(below)) {
        if (ui.pressed && !ui.touch && !(IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_LEFT_SHIFT))) browser_clear_selection();
        if (ui.rpressed && !ui.touch) ui_menu_open(app.menu_bg, (Rectangle){ui.mouse.x, ui.mouse.y, 0, 0});
    }
}

void view_draw(Rectangle r)
{
    ui_rect(r, theme.bg);
    if (br.error) {
        draw_empty(r, "triangle-alert", "This folder cannot be shown", br.error, 1);
        return;
    }
    if (!br.view.n) {
        if (br.filtering && br.filter.len) draw_empty(r, "search", "Nothing matches", NULL, 0);
        else draw_empty(r, "folder-open", "Empty folder", br.items.n ? "Only hidden items are here" : NULL, 0);
        return;
    }
    switch (app.layout) {
    default: draw_details(r); break;
    }
}

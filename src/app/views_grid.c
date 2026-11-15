#include "views.h"
#include <math.h>

static float tile_width(void) { return DP(ui.touch ? 104 : 120); }
static float grid_pad(void) { return DP(8); }
static float name_size(void) { return 13; }
static float tile_height(void) { return DP(10 + 52 + 6 + 10) + 2 * ui_line_height(name_size()); }

int grid_columns_for(float width)
{
    int n = (int)((width - 2 * grid_pad()) / tile_width());
    return n < 1 ? 1 : n;
}

void grid_reveal(int v, float width)
{
    float th = tile_height(), top = (v / grid_columns_for(width)) * th;
    ui_scroll_to(&br.scroll, top, top + th + 2 * grid_pad());
}

static int draw_tile(int v, Rectangle t, Rectangle area)
{
    fr_item *it = browser_item(v);
    Rectangle hit = GetCollisionRec(t, area);
    int state, clicked = ui_clickable(ui_id(it->e.name, (int)ui_id("tile", 0)), hit, &state);
    int right = ui_hover(hit) && ui.rpressed;
    Rectangle box = {t.x + DP(4), t.y + DP(4), t.width - DP(8), t.height - DP(8)};
    if (it->selected) ui_round(box, 8, theme.selection);
    else if (state) ui_round(box, 8, state == 2 ? theme.pressed : theme.hover);
    view_flash(it, box);
    if (!ui.touch && br.keyboard && v == br.cursor && !it->selected) ui_round_line(box, 8, ui_alpha(theme.accent, 0.5f));

    Rectangle icon = {t.x, box.y + DP(6), t.width, DP(52)};
    if (it->selected && ui.touch) ui_icon("square-check", icon, 40, theme.accent);
    else if (thumb_draw(it, (Rectangle){box.x + DP(6), icon.y, box.width - DP(12), icon.height}) <= 0)
        ui_icon(kind_icon(it->kind), icon, 48, kind_color(it->kind));
    if (it->e.is_link) ui_icon("link", (Rectangle){t.x + t.width / 2 + DP(12), icon.y + DP(34), DP(14), DP(14)}, 12, theme.text_faint);
    Rectangle name = {box.x + DP(6), icon.y + icon.height + DP(6), box.width - DP(12), 2 * ui_line_height(name_size())};
    if (keys_insert_field(v, (Rectangle){box.x + DP(2), name.y - DP(4), box.width - DP(4), DP(30)})) return ui_hover(hit);
    view_draw_name(it->e.name, name, name_size(), it->e.hidden ? theme.text_dim : theme.text);
    if (clicked) view_row_pressed(v, 0);
    else if (right) view_row_pressed(v, 1);
    else if (ui.mpressed && ui_hover(hit)) view_open_in_tab(v);
    return ui_hover(hit);
}

void grid_draw(Rectangle r)
{
    int cols = grid_columns_for(r.width), rows = (br.view.n + cols - 1) / cols;
    float pad = grid_pad(), tw = (r.width - 2 * pad) / cols, th = tile_height();
    ui_scroll(ui_id("grid", 0), &br.scroll, r, 2 * pad + rows * th + DP(ui.touch ? 96 : 0));
    int first = (int)((br.scroll.offset - pad) / th), last = (int)((br.scroll.offset + r.height) / th);
    if (first < 0) first = 0;
    if (last >= rows) last = rows - 1;
    int over_tile = 0;
    ui_clip_begin(r);
    for (int row = first; row <= last; row++)
        for (int c = 0; c < cols; c++) {
            int v = row * cols + c;
            if (v >= br.view.n) break;
            float x0 = roundf(r.x + pad + c * tw), x1 = roundf(r.x + pad + (c + 1) * tw);
            Rectangle t = {x0, roundf(r.y + pad + row * th - br.scroll.offset), x1 - x0, th};
            over_tile |= draw_tile(v, t, r);
        }
    ui_clip_end();
    float bar = br.scroll.content > br.scroll.view ? DP(14) : 0;
    if (!over_tile) view_background_input((Rectangle){r.x, r.y, r.width - bar, r.height});
}

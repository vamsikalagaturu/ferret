#include "views.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *uri;
    fr_entries es;
    int err, show_hidden;
    fr_scroll scroll;
} listing;

static listing parent, preview;

static int cmp_entries(const void *pa, const void *pb)
{
    const fr_entry *a = pa, *b = pb;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    int c = fr_natcmp(a->name, b->name);
    return c ? c : strcmp(a->name, b->name);
}

static void listing_clear(listing *l)
{
    free(l->uri);
    fr_entries_free(&l->es);
    *l = (listing){0};
}

/* 1 when it was (re)loaded */
static int listing_load(listing *l, const char *uri)
{
    if (l->uri && !strcmp(l->uri, uri) && l->show_hidden == br.show_hidden) return 0;
    listing_clear(l);
    l->uri = fr_strdup(uri);
    l->show_hidden = br.show_hidden;
    fr_entries all = {0};
    l->err = fr_vfs_list(uri, &all);
    for (int i = 0; i < all.n; i++) {
        if (all.v[i].hidden && !br.show_hidden) fr_entry_free(&all.v[i]);
        else FR_PUSH(l->es, all.v[i]);
    }
    free(all.v);
    if (l->es.n > 1) qsort(l->es.v, (size_t)l->es.n, sizeof *l->es.v, cmp_entries);
    ui.dirty = 1;
    return 1;
}

static void draw_entry(const fr_entry *e, Rectangle row)
{
    float pad = DP(12), icon_w = DP(32);
    fr_kind k = kind_of(e->name, e->is_dir);
    ui_icon(kind_icon(k), (Rectangle){row.x + pad, row.y, icon_w, row.height}, 20, kind_color(k));
    float tx = row.x + pad + icon_w + DP(8);
    ui_text_box(e->name, (Rectangle){tx, row.y, row.x + row.width - pad - tx, row.height}, 15, FONT_REGULAR,
                e->hidden ? theme.text_dim : theme.text, ALIGN_LEFT, 1);
    ui_hline(tx, row.y + row.height - DP(1), row.x + row.width - tx, ui_alpha(theme.border, 0.6f));
}

static void draw_parent(Rectangle r)
{
    char *up = fr_uri_parent(br.uri);
    if (!up) return;
    float rh = app_row_height();
    int fresh = listing_load(&parent, up);
    char *here = fr_uri_name(br.uri);
    int at = -1;
    for (int i = 0; i < parent.es.n; i++)
        if (!strcmp(parent.es.v[i].name, here)) at = i;
    free(here);
    if (fresh && at >= 0) parent.scroll.target = parent.scroll.offset = at * rh - (r.height - rh) / 2;
    ui_scroll(ui_id("parent", 0), &parent.scroll, r, parent.es.n * rh + DP(24));
    int first = (int)(parent.scroll.offset / rh), last = (int)((parent.scroll.offset + r.height) / rh), open = -1;
    if (last >= parent.es.n) last = parent.es.n - 1;
    ui_clip_begin(r);
    for (int i = first; i <= last; i++) {
        Rectangle row = {r.x, roundf(r.y + i * rh - parent.scroll.offset), r.width, rh};
        int state;
        if (ui_clickable(ui_id("parent-row", i), GetCollisionRec(row, r), &state)) open = i;
        if (i == at) ui_rect(row, theme.selection);
        else if (state) ui_rect(row, state == 2 ? theme.pressed : theme.hover);
        draw_entry(&parent.es.v[i], row);
    }
    ui_clip_end();
    if (open >= 0 && open != at) {
        const fr_entry *e = &parent.es.v[open];
        char *uri = fr_uri_join(up, e->name);
        ops_open_entry(uri, e);
        free(uri);
    }
    free(up);
}

static void draw_info_line(const char *label, const char *value, Rectangle r)
{
    ui_text_box(label, r, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    float lw = ui_text_width(label, 13, FONT_REGULAR) + DP(16);
    ui_text_box(value, (Rectangle){r.x + lw, r.y, r.width - lw, r.height}, 13, FONT_REGULAR, theme.text, ALIGN_RIGHT, 1);
}

static void draw_file_info(Rectangle r, const fr_item *it)
{
    float pad = DP(20), w = r.width - 2 * pad, x = r.x + pad, y = r.y + DP(32), lh = ui_line_height(15);
    /* the image's box stays while its thumbnail is on the way, so the text below does not jump */
    Rectangle image = {x, y, w, fminf(w, r.height * 0.45f)};
    int shown = thumb_draw(it, image);
    if (shown < 0) {
        ui_icon(kind_icon(it->kind), (Rectangle){r.x, y, r.width, DP(64)}, 48, kind_color(it->kind));
        y += DP(76);
    } else {
        if (!shown) ui_icon(kind_icon(it->kind), image, 48, kind_color(it->kind));
        y += image.height + DP(12);
    }
    view_draw_name(it->e.name, (Rectangle){x, y, w, 2 * lh}, 15, theme.text);
    y += 2 * lh + DP(16);
    ui_hline(x, y, w, theme.border);
    y += DP(8);
    char size[32], date[64];
    fr_format_size(it->e.size, size, sizeof size);
    time_t t = (time_t)it->e.mtime;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(date, sizeof date, "%d %b %Y %H:%M", &tm);
    float row_h = DP(28);
    draw_info_line("Kind", kind_label(it->kind), (Rectangle){x, y, w, row_h});
    draw_info_line("Size", size, (Rectangle){x, y + row_h, w, row_h});
    draw_info_line("Modified", date, (Rectangle){x, y + 2 * row_h, w, row_h});
}

static void draw_folder_preview(Rectangle r, const char *uri)
{
    if (listing_load(&preview, uri)) preview.scroll = (fr_scroll){0};
    if (preview.err) {
        view_draw_empty(r, "triangle-alert", "Cannot be read", fr_vfs_error(preview.err), 0);
        return;
    }
    if (!preview.es.n) {
        view_draw_empty(r, "folder-open", "Empty folder", NULL, 0);
        return;
    }
    float rh = app_row_height();
    ui_scroll(ui_id("preview", 0), &preview.scroll, r, preview.es.n * rh + DP(24));
    int first = (int)(preview.scroll.offset / rh), last = (int)((preview.scroll.offset + r.height) / rh);
    if (last >= preview.es.n) last = preview.es.n - 1;
    ui_clip_begin(r);
    for (int i = first; i <= last; i++) draw_entry(&preview.es.v[i], (Rectangle){r.x, roundf(r.y + i * rh - preview.scroll.offset), r.width, rh});
    ui_clip_end();
}

static void draw_preview(Rectangle r)
{
    fr_item *it = browser_item(br.cursor);
    if (!it || br.error) {
        listing_clear(&preview);
        return;
    }
    if (it->e.is_dir) {
        char *uri = fr_uri_join(br.uri, it->e.name);
        draw_folder_preview(r, uri);
        free(uri);
    } else {
        listing_clear(&preview);
        draw_file_info(r, it);
    }
}

void columns_draw(Rectangle r)
{
    float pw = roundf(r.width * 0.25f), cw = roundf(r.width * 0.40f), line = DP(1);
    Rectangle left = {r.x, r.y, pw - line, r.height};
    Rectangle mid = {r.x + pw, r.y, cw - line, r.height};
    Rectangle right = {r.x + pw + cw, r.y, r.width - pw - cw, r.height};
    ui_rect((Rectangle){r.x + pw - line, r.y, line, r.height}, theme.border);
    ui_rect((Rectangle){r.x + pw + cw - line, r.y, line, r.height}, theme.border);
    draw_preview(right);
    if (!view_draw_folder_state(mid)) view_list_draw(mid);
    draw_parent(left);
}

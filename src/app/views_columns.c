#define _GNU_SOURCE
#include "views.h"
#include "core/sniff.h"
#include "jobs/async.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *uri;
    fr_entries es;
    int err, show_hidden;
    int loading, arrived; /* a remote folder, listed on a thread of its own; its entries came since the last frame */
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

static void keep(listing *l, fr_entries *all, int rc)
{
    l->err = rc;
    for (int i = 0; i < all->n; i++) {
        if (all->v[i].hidden && !l->show_hidden) fr_entry_free(&all->v[i]);
        else FR_PUSH(l->es, all->v[i]);
    }
    free(all->v);
    *all = (fr_entries){0};
    if (l->es.n > 1) qsort(l->es.v, (size_t)l->es.n, sizeof *l->es.v, cmp_entries);
    ui.dirty = 1;
}

static void listed(const char *uri, fr_entries *all, int rc, void *ctx)
{
    listing *l = ctx;
    if (!l->loading || !l->uri || strcmp(l->uri, uri)) return;
    l->loading = 0, l->arrived = 1;
    keep(l, all, rc);
}

/* 1 when it was (re)loaded */
static int listing_load(listing *l, const char *uri)
{
    if (l->uri && !strcmp(l->uri, uri) && l->show_hidden == br.show_hidden) return 0;
    listing_clear(l);
    l->uri = fr_strdup(uri);
    l->show_hidden = br.show_hidden;
    if (!fr_uri_local(uri) && fr_vfs_backend(uri)) {
        l->loading = 1;
        net_prepare(uri);
        fr_async_list(uri, listed, l);
        return 1;
    }
    fr_entries all = {0};
    keep(l, &all, fr_vfs_list(uri, &all));
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
    int fresh = listing_load(&parent, up) || parent.arrived;
    parent.arrived = 0;
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
    char *uri = fr_uri_join(br.uri, it->e.name);
    int shown = preview_image_draw(uri, &it->e, image);
    free(uri);
    /* the thumbnail while the picture is on its way, or where there is none at this size */
    if (shown <= 0) {
        int thumb = thumb_draw(it, image);
        if (thumb > 0 || shown < 0) shown = thumb;
    }
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

/* ---- the first lines of a text file ---- */

#define TEXT_LINES 200
#define TEXT_READ (64 * 1024)
#define TEXT_LINE_MAX 400 /* bytes kept of a line: the pane clips it anyway */

static struct {
    char *uri;
    int64_t mtime;
    uint64_t size;
    int is_text;
    char *text; /* the lines, each NUL-terminated, tabs as spaces */
    int nlines;
    fr_scroll scroll;
} text_prev;

static void text_clear(void)
{
    free(text_prev.uri);
    free(text_prev.text);
    memset(&text_prev, 0, sizeof text_prev);
}

static int text_candidate(const fr_item *it)
{
    /* read while the frame is drawn: only where that is quick */
    if (it->e.is_dir || !it->e.size || !fr_uri_local(br.uri)) return 0;
    if (it->kind == KIND_CODE || it->kind == KIND_TEXT || it->kind == KIND_OTHER) return 1;
    return !strncmp(mime_of(it->e.name, 0), "text/", 5);
}

/* reads the start of uri; is_text stays 0 for a binary file or one that cannot be read */
static void text_load(const char *uri, const fr_item *it)
{
    text_clear();
    text_prev.uri = fr_strdup(uri);
    text_prev.mtime = it->e.mtime, text_prev.size = it->e.size;
    int err = 0;
    fr_stream *s = fr_vfs_open_read(uri, &err);
    if (!s) return;
    char *buf = fr_realloc(NULL, TEXT_READ);
    size_t n = 0;
    long got;
    while (n < TEXT_READ && (got = s->read(s, buf + n, TEXT_READ - n)) > 0) n += (size_t)got;
    s->close(s);
    int whole = n < TEXT_READ;
    size_t sniff = n < 8192 ? n : 8192;
    if (!fr_looks_text(buf, sniff, whole && sniff == n)) {
        free(buf);
        return;
    }
    if (!whole) {
        char *last = memrchr(buf, '\n', n);
        n = last ? (size_t)(last - buf) : fr_utf8_whole(buf, n);
    }
    struct {
        char *v;
        int n, cap;
    } out = {0};
    int lines = 0;
    for (size_t i = 0; i < n && lines < TEXT_LINES;) {
        size_t end = i;
        while (end < n && buf[end] != '\n') end++;
        size_t keep = fr_utf8_whole(buf + i, end - i < TEXT_LINE_MAX ? end - i : TEXT_LINE_MAX);
        for (size_t k = i; k < i + keep; k++) {
            unsigned char c = (unsigned char)buf[k];
            if (c == '\t') {
                for (int sp = 0; sp < 4; sp++) FR_PUSH(out, ' ');
            } else if (c != '\r') FR_PUSH(out, c < 0x20 || c == 0x7f ? ' ' : (char)c);
        }
        FR_PUSH(out, 0);
        lines++;
        i = end + 1;
    }
    free(buf);
    text_prev.text = out.v;
    text_prev.nlines = lines;
    text_prev.is_text = lines > 0;
}

static void draw_text_preview(Rectangle r, const fr_item *it)
{
    float pad = DP(16), x = r.x + pad, w = r.width - 2 * pad, y = r.y + DP(12);
    ui_icon(kind_icon(it->kind), (Rectangle){x - DP(4), y, DP(28), DP(24)}, 18, kind_color(it->kind));
    ui_text_box(it->e.name, (Rectangle){x + DP(28), y, w - DP(28), DP(24)}, 15, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 1);
    y += DP(26);
    char size[32], info[96];
    fr_format_size(it->e.size, size, sizeof size);
    snprintf(info, sizeof info, "%s  ·  %s%s", kind_label(it->kind), size,
             text_prev.nlines >= TEXT_LINES || it->e.size > TEXT_READ ? "  ·  the start" : "");
    ui_text_box(info, (Rectangle){x, y, w, DP(20)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    y += DP(28);
    ui_hline(r.x, y, r.width, theme.border);
    Rectangle body = {r.x, y + DP(1), r.width, r.y + r.height - y - DP(1)};
    float lh = ui_line_height(13);
    ui_scroll(ui_id("text-preview", 0), &text_prev.scroll, body, text_prev.nlines * lh + DP(24));
    int first = (int)(text_prev.scroll.offset / lh), last = (int)((text_prev.scroll.offset + body.height) / lh);
    ui_clip_begin(body);
    const char *line = text_prev.text;
    for (int i = 0; i < text_prev.nlines && i <= last; i++, line += strlen(line) + 1)
        if (i >= first) ui_text_box(line, (Rectangle){x, body.y + DP(8) + i * lh - text_prev.scroll.offset, w, lh}, 13, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
    ui_clip_end();
}

/* 1 when it drew the file's text */
static int text_preview(Rectangle r, const fr_item *it)
{
    if (!text_candidate(it)) {
        text_clear();
        return 0;
    }
    char *uri = fr_uri_join(br.uri, it->e.name);
    if (!text_prev.uri || strcmp(text_prev.uri, uri) || text_prev.mtime != it->e.mtime || text_prev.size != it->e.size)
        text_load(uri, it);
    free(uri);
    if (!text_prev.is_text) return 0;
    draw_text_preview(r, it);
    return 1;
}

static void draw_folder_preview(Rectangle r, const char *uri)
{
    if (listing_load(&preview, uri)) preview.scroll = (fr_scroll){0};
    if (preview.loading) {
        view_draw_empty(r, "loader-circle", "Loading...", NULL, 0);
        return;
    }
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
        text_clear();
        return;
    }
    if (it->e.is_dir) {
        text_clear();
        char *uri = fr_uri_join(br.uri, it->e.name);
        draw_folder_preview(r, uri);
        free(uri);
    } else {
        listing_clear(&preview);
        if (!text_preview(r, it)) draw_file_info(r, it);
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

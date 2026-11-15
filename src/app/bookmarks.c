/* Bookmarks as ranger and lf have them: m<key> marks the folder, '<key> goes there, '' back to the folder shown
 * before, "<key> forgets one. While a chord waits for its key the marks show over the files, as ranger shows them;
 * touch has a panel instead (the overflow menu's Bookmarks). They live in settings.bookmarks, saved by settings.c. */
#include "app.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int panel_open;
static fr_scroll panel_scroll;

static void set_now(const char *uri, int key)
{
    fr_bookmark_set(&settings.bookmarks, key, uri);
    const char *p = fr_uri_local(uri);
    ui_toast("Bookmark %c: %s", key, p ? p : uri);
}

static void remove_now(const char *s, int key)
{
    if (fr_bookmark_remove(&settings.bookmarks, key)) ui_toast("Bookmark %c removed", key);
}

static void go(const char *uri)
{
    char *to = fr_strdup(uri); /* the open replaces br.prev_uri, which uri may be */
    browser_open(to, 1);
    free(to);
}

void bookmark_key(int prefix, int key)
{
    if (prefix == '\'' && (key == '\'' || key == '`')) {
        if (br.prev_uri) go(br.prev_uri);
        else ui_toast("No folder before this one");
        return;
    }
    if (!fr_bookmark_key_valid(key)) {
        ui_toast("A bookmark's key is a letter or a digit");
        return;
    }
    const char *uri = fr_bookmark_get(&settings.bookmarks, key);
    if (prefix == 'm') {
        if (br.uri) app_defer(set_now, br.uri, key);
    } else if (!uri) ui_toast("No bookmark %c (m%c sets one)", key, key);
    else if (prefix == '\'') go(uri);
    else app_defer(remove_now, NULL, key);
}

void bookmarks_panel_open(void)
{
    panel_open = 1;
    panel_scroll = (fr_scroll){0};
    ui.dirty = 1;
}

int bookmarks_panel_is_open(void) { return panel_open; }

/* a local folder's path, "~" for home; anything else's URI */
static void shown(const char *uri, const char *home, char *out, size_t len)
{
    const char *p = fr_uri_local(uri);
    size_t hl = home ? strlen(home) : 0;
    if (p && hl > 1 && !strncmp(p, home, hl) && (!p[hl] || p[hl] == '/')) snprintf(out, len, "~%s", p + hl);
    else snprintf(out, len, "%s", p ? p : uri);
}

static void draw_row(int key, const char *uri, const char *home, Rectangle row)
{
    char k[2] = {(char)key, 0}, path[1024];
    shown(uri, home, path, sizeof path);
    float kw = DP(32);
    ui_text_box(k, (Rectangle){row.x, row.y, kw, row.height}, 15, FONT_SEMIBOLD, theme.accent, ALIGN_LEFT, 0);
    int here = br.uri && !strcmp(br.uri, uri);
    ui_text_box(path, (Rectangle){row.x + kw, row.y, row.width - kw, row.height}, 14, here ? FONT_SEMIBOLD : FONT_REGULAR,
                here ? theme.text : theme.text_dim, ALIGN_LEFT, 1);
}

/* ranger's list of marks, at the foot of the files while m, ' or " waits for its key */
static void draw_marks(Rectangle list, int prefix, const char *home)
{
    const fr_bookmarks *b = &settings.bookmarks;
    int prev = prefix == '\'' && br.prev_uri, rows = b->n + prev;
    float lh = DP(28), pad = DP(14), head = DP(26), margin = DP(16);
    int fit = (int)((list.height * 0.6f - 2 * pad - head) / lh);
    if (fit < 1) fit = 1;
    int shown_rows = rows > fit ? fit : rows, more = rows - shown_rows;
    float h = 2 * pad + head + (shown_rows + (more > 0) + (!rows)) * lh;
    /* above the chord's chip at the bottom right */
    Rectangle card = {list.x + margin, list.y + list.height - h - DP(48), list.width - 2 * margin, h};
    ui_round(card, 12, theme.surface_hi);
    ui_round_line(card, 12, theme.border);
    const char *title = prefix == 'm' ? "Bookmark this folder as: a letter or a digit"
                        : prefix == '\'' ? "Go to"
                                         : "Remove the bookmark";
    float x = card.x + pad, w = card.width - 2 * pad, y = card.y + pad;
    ui_text_box(title, (Rectangle){x, y, w, head}, 13, FONT_SEMIBOLD, theme.text_dim, ALIGN_LEFT, 0);
    y += head;
    if (!rows) ui_text_box("No bookmarks yet", (Rectangle){x, y, w, lh}, 14, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    int r = 0;
    if (prev && r < shown_rows) draw_row('\'', br.prev_uri, home, (Rectangle){x, y + r++ * lh, w, lh});
    for (int i = 0; i < b->n && r < shown_rows; i++) draw_row(b->v[i].key, b->v[i].uri, home, (Rectangle){x, y + r++ * lh, w, lh});
    if (more > 0) {
        char line[64];
        snprintf(line, sizeof line, "and %d more", more);
        ui_text_box(line, (Rectangle){x, y + r * lh, w, lh}, 13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    }
}

static void add_here_now(const char *uri, int n)
{
    int key = fr_bookmark_free_key(&settings.bookmarks);
    if (key) set_now(uri, key);
    else ui_toast("Every key has a bookmark: remove one first");
}

/* the list to tap: a row goes there, its x forgets it */
static void draw_panel(const char *home)
{
    const fr_bookmarks *b = &settings.bookmarks;
    ui_layer(1);
    ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.scrim);
    float pad = DP(ui.touch ? 20 : 24), rh = DP(ui.touch ? 52 : 40), bh = DP(40), head = DP(40);
    float w = fminf(ui.safe.width - DP(32), DP(560));
    float body_h = fmaxf(rh, b->n * rh), foot = bh + 2 * DP(16);
    float h = fminf(ui.safe.height - DP(48), pad + head + body_h + foot);
    Rectangle card = {ui.safe.x + (ui.safe.width - w) / 2, ui.safe.y + (ui.safe.height - h) / 2, w, h};
    ui_round(card, 14, theme.surface_hi);
    ui_round_line(card, 14, theme.border);
    float x = card.x + pad, inner = w - 2 * pad;
    ui_text_box("Bookmarks", (Rectangle){x, card.y + pad, inner, DP(28)}, 18, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    Rectangle body = {card.x, card.y + pad + head, w, h - pad - head - foot};
    ui_hline(card.x, body.y, w, theme.border);
    ui_hline(card.x, body.y + body.height, w, theme.border);
    ui_scroll(ui_id("bookmarks-list", 0), &panel_scroll, body, b->n * rh);
    ui_clip_begin(body);
    const char *go_to = NULL;
    int forget = 0;
    if (!b->n)
        ui_text_box(ui.touch ? "No bookmarks yet" : "No bookmarks yet: m and a letter or digit marks the folder",
                    (Rectangle){x, body.y, inner, rh}, 14, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    for (int i = 0; i < b->n; i++) {
        Rectangle row = {card.x, roundf(body.y + i * rh - panel_scroll.offset), w, rh};
        float xb = DP(ui.touch ? 48 : 36);
        Rectangle xr = {card.x + w - pad - xb, row.y + (rh - xb) / 2, xb, xb};
        int state;
        Rectangle hit = GetCollisionRec((Rectangle){row.x, row.y, xr.x - row.x, rh}, body);
        if (ui_clickable(ui_id("bookmark-row", b->v[i].key), hit, &state)) go_to = b->v[i].uri;
        if (state) ui_rect(hit, state == 2 ? theme.pressed : theme.hover);
        draw_row(b->v[i].key, b->v[i].uri, home, (Rectangle){x, row.y, xr.x - x - DP(8), rh});
        if (ui_icon_button(ui_id("bookmark-remove", b->v[i].key), xr, "x", theme.text_dim)) forget = b->v[i].key;
    }
    ui_clip_end();
    int close = 0;
    float by = card.y + h - DP(16) - bh, cw = ui_button_width("Close"), right = card.x + w - pad;
    int can_add = br.uri && !fr_bookmark_key_of(b, br.uri) && fr_bookmark_free_key(b);
    if (can_add) {
        float aw = ui_button_width("Bookmark this folder");
        if (ui_button(ui_id("bookmark-add", 0), (Rectangle){right - aw, by, aw, bh}, "Bookmark this folder", 1))
            app_defer(add_here_now, br.uri, 0);
        right -= aw + DP(8);
    }
    if (ui_button(ui_id("bookmarks-close", 0), (Rectangle){right - cw, by, cw, bh}, "Close", !can_add)) close = 1;
    if (go_to) go(go_to), close = 1;
    if (forget) app_defer(remove_now, NULL, forget);
    if (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)) close = 1;
    if (ui_live() && ui.pressed && !CheckCollisionPointRec(ui.mouse, card)) close = 1;
    if (close) panel_open = 0, ui.dirty = 1;
    ui_layer(0);
}

void bookmarks_draw(Rectangle list)
{
    const char *chord = keys_pending();
    int prefix = chord[0] == 'm' || chord[0] == '\'' || chord[0] == '"' ? chord[0] : 0;
    if (!prefix && !panel_open) return;
    char *home_uri = plat_home_uri();
    const char *home = home_uri ? fr_uri_local(home_uri) : NULL;
    if (panel_open) draw_panel(home);
    else if (!dview_is_open()) draw_marks(list, prefix, home);
    free(home_uri);
}

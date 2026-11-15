/* A short list to pick one row from, one of ops.c's dialogs: the recent folders (g-) and the clipboard's history
 * (yh). j/k or the arrows move, Enter or a click picks, 1 to 9 pick that row, Esc closes. */
#include "app.h"
#include "net/net.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int open;
    char title[64];
    fr_pick *rows;
    int n, sel;
    void (*chosen)(const char *value, int index);
    fr_scroll scroll;
} P;

static void rows_free(fr_pick *rows, int n)
{
    for (int i = 0; i < n; i++) free(rows[i].label), free(rows[i].detail), free(rows[i].value);
    free(rows);
}

void picker_close(void)
{
    rows_free(P.rows, P.n);
    memset(&P, 0, sizeof P);
    ui.dirty = 1;
}

int picker_open(const char *title, fr_pick *rows, int n, void (*chosen)(const char *value, int index))
{
    picker_close();
    if (!n) {
        rows_free(rows, n);
        return 0;
    }
    snprintf(P.title, sizeof P.title, "%s", title);
    P.rows = rows, P.n = n, P.chosen = chosen, P.open = 1;
    return 1;
}

static float row_height(void) { return fmaxf(app_row_height(), ui_line_height(15) + ui_line_height(12) + DP(16)); }

static void select_row(int i)
{
    P.sel = i < 0 ? 0 : i >= P.n ? P.n - 1 : i;
    float rh = row_height();
    ui_scroll_to(&P.scroll, P.sel * rh, (P.sel + 1) * rh);
    ui.dirty = 1;
}

int picker_draw(void)
{
    if (!P.open) return 0;
    ui_layer(1);
    ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.scrim);
    Rectangle s = ui.safe;
    float pad = DP(24), rh = row_height(), head = DP(44);
    float w = fminf(s.width - DP(32), DP(520)), h = fminf(s.height - DP(48), pad + head + fminf((float)P.n, 10) * rh + pad);
    Rectangle card = {s.x + (s.width - w) / 2, s.y + (s.height - h) / 2, w, h};
    ui_round(card, 14, theme.surface_hi);
    ui_round_line(card, 14, theme.border);
    float x = card.x + pad, inner = w - 2 * pad;
    ui_text_box(P.title, (Rectangle){x, card.y + pad, inner, DP(26)}, 18, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    Rectangle list = {card.x, card.y + pad + head, w, card.y + h - pad / 2 - (card.y + pad + head)};
    ui_hline(card.x, list.y, w, theme.border);
    ui_scroll(ui_id("pick-scroll", 0), &P.scroll, list, P.n * rh);
    int pick = -1;
    ui_clip_begin(list);
    for (int i = 0; i < P.n; i++) {
        Rectangle row = {list.x, roundf(list.y + i * rh - P.scroll.offset), list.width, rh};
        if (row.y + rh < list.y || row.y > list.y + list.height) continue;
        int state;
        if (ui_clickable(ui_id("pick-row", i), GetCollisionRec(row, list), &state)) pick = i;
        if (i == P.sel) ui_rect(row, theme.selection);
        else if (state) ui_rect(row, state == 2 ? theme.pressed : theme.hover);
        ui_icon(P.rows[i].icon ? P.rows[i].icon : "folder", (Rectangle){x - DP(4), row.y, DP(32), rh}, 20,
                i == P.sel ? theme.accent : theme.text_dim);
        float right = x + inner;
        if (!ui.touch && i < 9) {
            char digit[12];
            snprintf(digit, sizeof digit, "%d", i + 1);
            ui_text_box(digit, (Rectangle){right - DP(16), row.y, DP(16), rh}, 13, FONT_REGULAR, theme.text_faint, ALIGN_RIGHT, 0);
            right -= DP(24);
        }
        float tx = x + DP(36), lh = ui_line_height(15), dh = ui_line_height(12), ty = row.y + (rh - lh - dh - DP(2)) / 2;
        ui_text_box(P.rows[i].label, (Rectangle){tx, ty, right - tx, lh}, 15, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
        if (P.rows[i].detail)
            ui_text_box(P.rows[i].detail, (Rectangle){tx, ty + lh + DP(2), right - tx, dh}, 12, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 1);
    }
    ui_clip_end();
    int open = 1;
    if (ui_key(KEY_DOWN, 0)) select_row(P.sel + 1);
    if (ui_key(KEY_UP, 0)) select_row(P.sel - 1);
    if (ui_key(KEY_HOME, 0)) select_row(0);
    if (ui_key(KEY_END, 0)) select_row(P.n - 1);
    for (int i = 0; i < ui.nchars; i++) {
        int c = ui.chars[i];
        if (c == 'j') select_row(P.sel + 1);
        else if (c == 'k') select_row(P.sel - 1);
        else if (c == 'l') pick = P.sel;
        else if (c == 'q') open = 0;
        else if (c >= '1' && c <= '9' && c - '1' < P.n) pick = c - '1';
    }
    ui.nchars = 0;
    if (ui_key(KEY_ENTER, 0) || ui_key(KEY_KP_ENTER, 0)) pick = P.sel;
    if (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)) open = 0;
    if (ui_live() && ui.pressed && !CheckCollisionPointRec(ui.mouse, card)) open = 0;
    ui_layer(0);
    if (open && pick >= 0) {
        app_defer(P.chosen, P.rows[pick].value, pick);
        open = 0;
    }
    if (!open) picker_close();
    return P.open;
}

/* ---- the recent folders ---- */

static void open_recent(const char *uri, int index) { browser_open(uri, 1); }

void recent_pick(void)
{
    fr_pick *rows = fr_realloc(NULL, sizeof *rows * (size_t)(settings.recent.n + 1));
    int n = 0;
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    for (int i = 0; i < settings.recent.n; i++) {
        const char *uri = settings.recent.v[i], *path = fr_uri_local(uri);
        if (br.uri && !strcmp(uri, br.uri)) continue;
        fr_pick *r = &rows[n++];
        char *name = fr_uri_name(uri);
        if (path) {
            r->label = *name ? fr_strdup(name) : fr_strdup("/");
            r->detail = hl > 1 && !strncmp(path, home, hl) && (path[hl] == '/' || !path[hl]) ? fr_format("~%s", path + hl) : fr_strdup(path);
            r->icon = "folder";
        } else {
            char *server = fr_net_label(uri);
            r->label = *name ? fr_strdup(name) : fr_strdup(server ? server : uri);
            r->detail = fr_strdup(uri);
            r->icon = fr_archive_is_uri(uri) ? "folder-archive" : !strncmp(uri, "trash:", 6) ? "trash-2" : "server";
            free(server);
        }
        r->value = fr_strdup(uri);
        free(name);
    }
    if (!n) {
        free(rows);
        ui_toast("No other folders visited yet");
        return;
    }
    ops_pick("Recent folders", rows, n, open_recent);
}

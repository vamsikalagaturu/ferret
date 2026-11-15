/* "Open with": the apps that take the chosen files' type, the default first, one of ops.c's dialogs. Android hands
 * the file to the system's chooser instead. */
#include "app.h"
#include "core/sniff.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void fr_app_choices_free(fr_app_choices *c)
{
    for (int i = 0; i < c->n; i++) free(c->v[i].id), free(c->v[i].name);
    free(c->v);
    *c = (fr_app_choices){0};
}

static struct {
    int open;
    fr_strs paths;
    char *mime;
    char subtitle[320];
    fr_app_choices apps;
    int sel, always;
    fr_scroll scroll;
} W;

/* by the extension; a file without a known one that reads as text is text/plain */
static char *type_of(const char *path, const char *name)
{
    const char *m = mime_of(name, 0);
    if (strcmp(m, "application/octet-stream")) return fr_strdup(m);
    char buf[8192];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf) : -1;
    if (fd >= 0) close(fd);
    return fr_strdup(n > 0 && fr_looks_text(buf, (size_t)n, n < (ssize_t)sizeof buf) ? "text/plain" : m);
}

void openwith_close(void)
{
    fr_strs_free(&W.paths);
    fr_app_choices_free(&W.apps);
    free(W.mime);
    memset(&W, 0, sizeof W);
    ui.dirty = 1;
}

static int show(void);

/* files elsewhere are copied here first (fetch.c), which opens the dialog once they are */
int openwith_open(void)
{
    openwith_close();
    fr_strs uris = browser_target_uris(), files = {0};
    int elsewhere = 0;
    for (int i = 0; i < uris.n; i++) {
        const char *p = fr_uri_local(uris.v[i]);
        fr_entry e = {0};
        if (p && !fr_vfs_stat(uris.v[i], &e) && !e.is_dir) FR_PUSH(files, fr_strdup(uris.v[i]));
        else if (!p && fr_vfs_backend(uris.v[i])) {
            fr_item *it = browser_item_of(uris.v[i]);
            if (!it || !it->e.is_dir) {
                FR_PUSH(files, fr_strdup(uris.v[i]));
                elsewhere = 1;
            }
        }
        fr_entry_free(&e);
    }
    fr_strs_free(&uris);
    if (!files.n) {
        fr_strs_free(&files);
        ui_toast("Open with takes files, not folders");
        return 0;
    }
    if (elsewhere) {
        fetch_open(&files, 1);
        fr_strs_free(&files);
        return 0;
    }
    for (int i = 0; i < files.n; i++) FR_PUSH(W.paths, fr_strdup(fr_uri_local(files.v[i])));
    fr_strs_free(&files);
    return show();
}

int openwith_open_paths(fr_strs *paths)
{
    openwith_close();
    W.paths = *paths;
    *paths = (fr_strs){0};
    return W.paths.n ? show() : 0;
}

static int show(void)
{
    const char *slash = strrchr(W.paths.v[0], '/'), *name = slash ? slash + 1 : W.paths.v[0];
    W.mime = type_of(W.paths.v[0], name);
    if (plat_apps_for(W.mime, &W.apps) < 0) {
        if (plat_open_chooser(W.paths.v[0])) ui_toast("No app can open %s", name);
        openwith_close();
        return 0;
    }
    if (!W.apps.n) {
        ui_toast("No app is set up to open %s", W.mime);
        openwith_close();
        return 0;
    }
    if (W.paths.n == 1) snprintf(W.subtitle, sizeof W.subtitle, "%s  ·  %s", name, W.mime);
    else snprintf(W.subtitle, sizeof W.subtitle, "%d files  ·  %s", W.paths.n, W.mime);
    W.open = 1;
    return 1;
}

static void launch(void)
{
    const fr_app_choice *c = &W.apps.v[W.sel];
    if (plat_open_app(c->id, &W.paths)) ui_toast("Could not start %s", c->name);
    else if (W.always && !c->is_default) {
        int rc = plat_set_default_app(W.mime, c->id);
        if (rc) ui_toast("Could not make %s the default: %s", c->name, strerror(-rc));
        else ui_toast("%s now opens %s", c->name, W.mime);
    }
    openwith_close();
}

static void select_app(int i)
{
    if (i < 0) i = 0;
    if (i >= W.apps.n) i = W.apps.n - 1;
    W.sel = i;
    float rh = app_row_height();
    ui_scroll_to(&W.scroll, i * rh, (i + 1) * rh);
    ui.dirty = 1;
}

int openwith_draw(void)
{
    if (!W.open) return 0;
    ui_layer(1);
    ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.scrim);
    Rectangle s = ui.safe;
    float pad = DP(24), rh = app_row_height(), head = DP(60), check_h = DP(40), bh = DP(40), foot = bh + 2 * DP(16);
    float list_h = fminf((float)W.apps.n, 8) * rh, w = fminf(s.width - DP(32), DP(480));
    float h = fminf(s.height - DP(48), pad + head + list_h + DP(8) + check_h + foot);
    Rectangle card = {s.x + (s.width - w) / 2, s.y + (s.height - h) / 2, w, h};
    ui_round(card, 14, theme.surface_hi);
    ui_round_line(card, 14, theme.border);
    float x = card.x + pad, inner = w - 2 * pad;
    ui_text_box("Open with", (Rectangle){x, card.y + pad, inner, DP(26)}, 18, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    ui_text_box(W.subtitle, (Rectangle){x, card.y + pad + DP(28), inner, DP(20)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 1);

    Rectangle list = {card.x, card.y + pad + head, w, card.y + h - foot - check_h - DP(8) - (card.y + pad + head)};
    ui_hline(card.x, list.y, w, theme.border);
    ui_scroll(ui_id("ow-scroll", 0), &W.scroll, list, W.apps.n * rh);
    int launch_now = 0;
    ui_clip_begin(list);
    for (int i = 0; i < W.apps.n; i++) {
        Rectangle row = {list.x, roundf(list.y + i * rh - W.scroll.offset), list.width, rh};
        if (row.y + rh < list.y || row.y > list.y + list.height) continue;
        int state;
        if (ui_clickable(ui_id("ow-app", i), GetCollisionRec(row, list), &state)) {
            if (ui.clicks == 2 && W.sel == i) launch_now = 1;
            W.sel = i;
        }
        if (i == W.sel) ui_rect(row, theme.selection);
        else if (state) ui_rect(row, state == 2 ? theme.pressed : theme.hover);
        ui_icon("app-window", (Rectangle){x - DP(4), row.y, DP(32), rh}, 20, i == W.sel ? theme.accent : theme.text_dim);
        float right = x + inner;
        if (W.apps.v[i].is_default) {
            float cw = ui_text_width("Default", 12, FONT_SEMIBOLD) + DP(16), ch = DP(22);
            Rectangle chip = {right - cw, row.y + (rh - ch) / 2, cw, ch};
            ui_round(chip, 6, ui_alpha(theme.accent, 0.18f));
            ui_text_box("Default", chip, 12, FONT_SEMIBOLD, theme.accent, ALIGN_CENTER, 0);
            right = chip.x - DP(8);
        }
        ui_text_box(W.apps.v[i].name, (Rectangle){x + DP(36), row.y, right - x - DP(36), rh}, 15, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
    }
    ui_clip_end();
    ui_hline(card.x, list.y + list.height, w, theme.border);

    Rectangle cell = {x, list.y + list.height + DP(8), DP(24), check_h};
    Rectangle hit = {x - DP(4), cell.y, inner + DP(8), check_h};
    int cs;
    if (ui_clickable(ui_id("ow-always", 0), hit, &cs)) W.always = !W.always;
    if (cs) ui_round(hit, 6, cs == 2 ? theme.pressed : theme.hover);
    ui_icon(W.always ? "square-check" : "square", cell, 20, W.always ? theme.accent : theme.text_dim);
    char always[200];
    snprintf(always, sizeof always, "Always use for %s", W.mime);
    ui_text_box(always, (Rectangle){x + DP(32), cell.y, inner - DP(32), check_h}, 14, FONT_REGULAR, theme.text, ALIGN_LEFT, 1);

    float by = card.y + h - DP(16) - bh, ow = ui_button_width("Open"), cw = ui_button_width("Cancel");
    int open = 1;
    if (ui_button(ui_id("ow-open", 0), (Rectangle){card.x + w - pad - ow, by, ow, bh}, "Open", 1)) launch_now = 1;
    if (ui_button(ui_id("ow-cancel", 0), (Rectangle){card.x + w - pad - ow - DP(8) - cw, by, cw, bh}, "Cancel", 0)) open = 0;
    if (ui_key(KEY_DOWN, 0)) select_app(W.sel + 1);
    if (ui_key(KEY_UP, 0)) select_app(W.sel - 1);
    for (int i = 0; i < ui.nchars; i++) {
        if (ui.chars[i] == 'j') select_app(W.sel + 1);
        if (ui.chars[i] == 'k') select_app(W.sel - 1);
    }
    ui.nchars = 0;
    if (ui_key(KEY_ENTER, 0) || ui_key(KEY_KP_ENTER, 0)) launch_now = 1;
    if (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)) open = 0;
    if (ui_live() && ui.pressed && !CheckCollisionPointRec(ui.mouse, card)) open = 0;
    ui_layer(0);
    if (launch_now && open) launch();
    else if (!open) openwith_close();
    return W.open;
}

#include "app.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void panes_init(void)
{
    br_cur = browser_new(NULL);
    tabs_add(&tabs, br_cur, 1);
    tabs_sync();
}

int app_split(void) { return tabs_split_now(&tabs); }

/* ---- the changes, run between frames (app_defer); br is the pane that asked ---- */

static void tab_new_now(const char *uri, int select)
{
    fr_browser *from = br_cur, *b = browser_new(from);
    char *at = fr_strdup(uri ? uri : from->uri);
    tabs_add(&tabs, b, select);
    if (at) {
        browser_enter(b);
        browser_open(at, 0);
    }
    free(at);
    tabs_sync();
    if (select) keys_reset();
    ui.dirty = 1;
}

void tab_new(const char *uri, int select) { app_defer(tab_new_now, uri, select); }

static void tab_close_now(const char *s, int i)
{
    if (i < 0) i = tabs.cur;
    fr_browser *gone[2];
    if (tabs_close(&tabs, i, gone)) return;
    tabs_sync();
    browser_free(gone[0]);
    browser_free(gone[1]);
    keys_reset();
    ui.dirty = 1;
}

void tab_close(int i) { app_defer(tab_close_now, NULL, i); }

static void tab_select_now(const char *s, int i)
{
    if (i == tabs.cur || i < 0 || i >= tabs.n) return;
    tabs_select(&tabs, i);
    tabs_sync();
    keys_reset();
    ui.dirty = 1;
}

void tab_select(int i)
{
    if (i >= tabs.n) ui_toast("No tab %d", i + 1);
    else app_defer(tab_select_now, NULL, i);
}

static void tab_step_now(const char *s, int by)
{
    if (tabs.n < 2) return;
    tabs_step(&tabs, by);
    tabs_sync();
    keys_reset();
    ui.dirty = 1;
}

void tab_step(int by) { app_defer(tab_step_now, NULL, by); }

static void split_now(const char *s, int n)
{
    if (tabs_split_now(&tabs)) {
        browser_free(tabs_unsplit(&tabs));
    } else {
        fr_browser *b = browser_new(br_cur);
        char *at = br_cur->uri ? fr_strdup(br_cur->uri) : NULL;
        tabs_split(&tabs, b);
        if (at) {
            browser_enter(b);
            browser_open(at, 0);
        }
        free(at);
    }
    tabs_sync();
    keys_reset();
    ui.dirty = 1;
}

void split_toggle(void)
{
    if (ui.touch) return;
    app_defer(split_now, NULL, 0);
}

static void pane_switch_now(const char *s, int n)
{
    tabs_switch_side(&tabs);
    tabs_sync();
    keys_reset();
    ui.dirty = 1;
}

void pane_switch(void)
{
    if (app_split()) app_defer(pane_switch_now, NULL, 0);
}

/* br, a pane of the current tab, gets the keys */
static void pane_activate_now(const char *s, int n)
{
    int side, t = tabs_find(&tabs, br_cur, &side);
    if (t != tabs.cur || tabs.v[t].side == side) return;
    tabs.v[t].side = side;
    tabs_sync();
    keys_reset();
    ui.dirty = 1;
}

/* ---- labels ---- */

static char *folder_label(const fr_browser *b)
{
    if (!b->uri) return fr_strdup("...");
    char *home = plat_home_uri();
    int is_home = home && !strcmp(home, b->uri);
    free(home);
    if (is_home) return fr_strdup("~");
    char *name = fr_uri_name(b->uri);
    if (*name) return name;
    free(name);
    return fr_strdup(fr_uri_local(b->uri) ? "/" : b->uri);
}

char *tab_label(int i)
{
    if (i < 0 || i >= tabs.n) return fr_strdup("");
    const fr_tab *t = &tabs.v[i];
    char *a = folder_label(t->pane[0]);
    if (!t->pane[1]) return a;
    char *b = folder_label(t->pane[1]), *both = fr_format("%s | %s", a, b);
    free(a), free(b);
    return both;
}

/* ---- the tab bar (desktop, two tabs or more) ---- */

float tabbar_height(void) { return ui.touch || tabs.n < 2 ? 0 : fmaxf(DP(36), ui_line_height(14) + DP(16)); }

void tabbar_draw(Rectangle r)
{
    ui_rect(r, theme.surface);
    float pad = DP(6), gap = DP(4), plus = DP(28), close = DP(22);
    float room = r.width - 2 * pad - plus - gap;
    /* past what fits, the last tabs are cut off: Alt+n, gt and the overflow menu still reach them */
    float w = fmaxf(DP(56), fminf(DP(220), (room - gap * (float)(tabs.n - 1)) / (float)tabs.n));
    float x = r.x + pad;
    Rectangle clip = {r.x, r.y, pad + room, r.height};
    ui_clip_begin(clip);
    for (int i = 0; i < tabs.n && x < clip.x + clip.width; i++, x += w + gap) {
        Rectangle t = {roundf(x), r.y + DP(4), roundf(w), r.height - DP(8)}, hit = GetCollisionRec(t, clip);
        int on = i == tabs.cur, state;
        int picked = ui_clickable(ui_id("tab", i), hit, &state);
        if (ui_live() && ui.mpressed && ui_hover(hit)) tab_close(i);
        Color bg = on ? ui_mix(theme.surface, theme.accent, 0.22f) : state == 2 ? theme.pressed : state ? theme.hover : theme.surface;
        ui_round(t, 6, bg);
        float ix = t.x + DP(6), iw = DP(20);
        ui_icon(tabs.v[i].pane[1] ? "columns-2" : "folder", (Rectangle){ix, t.y, iw, t.height}, 15, on ? theme.accent : theme.text_dim);
        char *label = tab_label(i);
        float tx = ix + iw + DP(6);
        ui_text_box(label, (Rectangle){tx, t.y, t.x + t.width - close - DP(4) - tx, t.height}, 14, on ? FONT_SEMIBOLD : FONT_REGULAR,
                    on ? theme.text : theme.text_dim, ALIGN_LEFT, 1);
        free(label);
        Rectangle xb = {t.x + t.width - close - DP(4), t.y + (t.height - close) / 2, close, close};
        if (ui_icon_button(ui_id("tab-close", i), xb, "x", theme.text_dim)) tab_close(i);
        else if (picked) tab_select(i);
    }
    ui_clip_end();
    Rectangle pb = {roundf(fminf(x, r.x + pad + room + gap)), r.y + (r.height - plus) / 2, plus, plus};
    if (ui_icon_button(ui_id("tab-new", 0), pb, "plus", theme.text_dim)) tab_new(NULL, 1);
    ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
}

/* ---- the panes ---- */

static void draw_pane(Rectangle r)
{
    float hh = header_height();
    view_draw((Rectangle){r.x, r.y + hh, r.width, r.height - hh});
    header_draw((Rectangle){r.x, r.y, r.width, hh});
}

void panes_draw(Rectangle r)
{
    const fr_tab *tab = &tabs.v[tabs.cur];
    if (!tab->pane[1]) {
        app.pane_rect[0] = r;
        app.pane_rect[1] = (Rectangle){0};
        draw_pane(r);
        return;
    }
    float half = roundf(r.width / 2), line = DP(1);
    app.pane_rect[0] = (Rectangle){r.x, r.y, half - line, r.height};
    app.pane_rect[1] = (Rectangle){r.x + half, r.y, r.width - half, r.height};
    ui_rect((Rectangle){r.x + half - line, r.y, line, r.height}, theme.border);
    for (int s = 0; s < 2; s++) {
        Rectangle pr = app.pane_rect[s];
        int active = s == tab->side;
        browser_enter(tab->pane[s]);
        if (!active && ui_live() && (ui.pressed || ui.rpressed) && ui_hover(pr)) app_defer(pane_activate_now, NULL, 0);
        draw_pane(pr);
        /* the pane without the keys recedes; the one with them has a tinted header (header.c) */
        if (!active) ui_rect(pr, ui_alpha(theme.bg, 0.35f));
    }
    tabs_sync();
}

/* ---- the overflow menu ---- */

void tabs_menu_items(void)
{
    if (ui_menu_item("New tab", "plus", -1)) tab_new(NULL, 1);
    /* the tab bar shows them on the desktop */
    if (ui.touch && tabs.n > 1)
        for (int i = 0; i < tabs.n; i++) {
            char *label = tab_label(i);
            if (ui_menu_item(label, "folder", i == tabs.cur)) tab_select(i);
            free(label);
        }
    if (tabs.n > 1 && ui_menu_item("Close tab", "x", -1)) tab_close(-1);
    if (!ui.touch && ui_menu_item("Split view", "columns-2", app_split())) split_toggle();
}

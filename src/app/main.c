#define _GNU_SOURCE
#include "app.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

fr_app app;

float app_row_height(void) { return DP(ui.touch ? 64 : 34); }
int app_wide(void) { return !ui.touch && ui.safe.width >= DP(840); }

/* ---- bottom bars ---- */

static float draw_paste_bar(Rectangle area)
{
    if (!clip.uris.n || br.nselected) return 0;
    float h = DP(ui.touch ? 64 : 52), pad = DP(12);
    Rectangle r = {area.x, area.y + area.height - h, area.width, h};
    ui_rect(r, theme.surface);
    ui_hline(r.x, r.y, r.width, theme.border);
    ui_icon(clip.cut ? "scissors" : "clipboard-paste", (Rectangle){r.x + pad, r.y, DP(32), h}, 20, theme.accent);
    char label[64];
    snprintf(label, sizeof label, "%d %s to %s", clip.uris.n, clip.uris.n == 1 ? "item" : "items", clip.cut ? "move" : "copy");
    float pw = ui_button_width("Paste here"), cw = ui_button_width("Cancel"), bh = DP(40);
    ui_text_box(label, (Rectangle){r.x + pad + DP(40), r.y, r.width - 2 * pad - DP(48) - pw - cw - DP(8), h}, 15, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
    float bx = r.x + r.width - pad - pw;
    if (ui_button(ui_id("paste", 0), (Rectangle){bx, r.y + (h - bh) / 2, pw, bh}, "Paste here", 1)) ops_paste();
    if (ui_button(ui_id("paste-cancel", 0), (Rectangle){bx - DP(8) - cw, r.y + (h - bh) / 2, cw, bh}, "Cancel", 0)) fr_strs_free(&clip.uris);
    return h;
}

/* ---- menus ---- */

static void draw_menus(void)
{
    if (ui_menu_begin(app.menu_more)) {
        if (ui_menu_item("New folder", "folder-plus", -1)) ops_new_folder_start();
        if (ui_menu_item("Select all", "check-check", -1)) browser_select_all();
        if (clip.uris.n && ui_menu_item("Paste", "clipboard-paste", -1)) ops_paste();
        ui_menu_separator();
        static const char *sorts[] = {"Sort by name", "Sort by size", "Sort by date", "Sort by kind"};
        for (int i = 0; i < 4; i++)
            if (ui_menu_item(sorts[i], NULL, br.sort == (fr_sort)i)) br.sort = (fr_sort)i, browser_rebuild_view();
        if (ui_menu_item("Reverse order", NULL, br.desc)) br.desc = !br.desc, browser_rebuild_view();
        if (ui_menu_item("Show hidden", NULL, br.show_hidden)) br.show_hidden = !br.show_hidden, browser_rebuild_view();
        ui_menu_separator();
        if (ui_menu_item("Refresh", "refresh-cw", -1)) browser_reload();
        ui_menu_end();
    }
    if (ui_menu_begin(app.menu_row)) {
        fr_item *it = browser_item(br.cursor);
        if (br.nselected == 1 && it && ui_menu_item("Open", "external-link", -1)) ops_open_item(it);
        if (ui_menu_item("Copy", "copy", -1)) ops_copy(0);
        if (ui_menu_item("Cut", "scissors", -1)) ops_copy(1);
        if (br.nselected == 1 && ui_menu_item("Rename", "pencil", -1)) ops_rename_start();
        ui_menu_separator();
        if (br.uri && fr_vfs_can_trash(br.uri) && ui_menu_item("Move to trash", "trash-2", -1)) ops_delete(0);
        if (ui_menu_item("Delete permanently", "trash-2", -1)) ops_delete(1);
        ui_menu_separator();
        if (ui_menu_item("Properties", "info", -1)) ops_properties();
        ui_menu_end();
    }
    if (ui_menu_begin(app.menu_bg)) {
        if (ui_menu_item("New folder", "folder-plus", -1)) ops_new_folder_start();
        if (clip.uris.n && ui_menu_item("Paste", "clipboard-paste", -1)) ops_paste();
        if (ui_menu_item("Select all", "check-check", -1)) browser_select_all();
        if (ui_menu_item("Show hidden", NULL, br.show_hidden)) br.show_hidden = !br.show_hidden, browser_rebuild_view();
        if (ui_menu_item("Refresh", "refresh-cw", -1)) browser_reload();
        ui_menu_end();
    }
    if (ui_menu_begin(app.menu_selection)) {
        if (br.nselected == 1 && ui_menu_item("Rename", "pencil", -1)) ops_rename_start();
        if (ui_menu_item("Properties", "info", -1)) ops_properties();
        if (br.uri && fr_vfs_can_trash(br.uri) && ui_menu_item("Delete permanently", "trash-2", -1)) ops_delete(1);
        ui_menu_end();
    }
}

/* ---- Android's storage permission ---- */

static void draw_permission(Rectangle r)
{
    ui_rect(r, theme.bg);
    float w = fminf(r.width - DP(48), DP(420)), x = r.x + (r.width - w) / 2, y = r.y + r.height * 0.22f;
    ui_icon("shield-alert", (Rectangle){x, y, w, DP(64)}, 56, theme.accent);
    y += DP(80);
    ui_text_box("ferret needs to see your files", (Rectangle){x, y, w, DP(28)}, 20, FONT_SEMIBOLD, theme.text, ALIGN_CENTER, 0);
    y += DP(40);
    const char *msg = "Android lets file managers work with all your files only after you turn on \"Allow access to manage "
                      "all files\" for them. ferret uses it to show, copy, move and delete files on this device, and for "
                      "nothing else.";
    int lines = ui_text_wrapped(msg, (Rectangle){x, y, w, 0}, 15, FONT_REGULAR, theme.text_dim, 1);
    y += lines * ui_line_height(15) + DP(24);
    float bw = ui_button_width("Allow access");
    if (ui_button(ui_id("grant", 0), (Rectangle){x + (w - bw) / 2, y, bw, DP(44)}, "Allow access", 1)) plat_request_storage();
}

/* ---- the frame ---- */

static void frame(void)
{
    static double checked_at = -10;
    if (!app.storage_ok && ui.now - checked_at > 0.5) {
        checked_at = ui.now;
        app.storage_ok = plat_storage_granted();
        if (app.storage_ok) {
            places_refresh(1);
            browser_reload();
        }
    }
    ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.bg);
    if (!app.storage_ok) {
        draw_permission(ui.safe);
        if (ui_key(KEY_BACK, 0)) app.quit = 1;
        ui_redraw_in(0.5);
        return;
    }
    if (app_wide() || app.drawer_open) places_refresh(0);
    int modal = ops_dialog_open() || ui_menu_is_open();
    Rectangle s = ui.safe;
    if (app_wide()) {
        float sw = DP(240);
        places_draw((Rectangle){s.x, s.y, sw, s.height});
        ui_rect((Rectangle){s.x + sw - DP(1), s.y, DP(1), s.height}, theme.border);
        s.x += sw, s.width -= sw;
    }
    float hh = header_height();
    Rectangle list = {s.x, s.y + hh, s.width, s.height - hh};
    list.height -= ops_draw_jobs(list);
    list.height -= draw_paste_bar(list);
    view_draw(list);
    const char *chord = keys_pending();
    if (*chord) {
        float cw = ui_text_width(chord, 13, FONT_REGULAR) + DP(20), ch = DP(24);
        Rectangle chip = {list.x + list.width - cw - DP(16), list.y + list.height - ch - DP(12), cw, ch};
        ui_round(chip, 12, theme.surface_hi);
        ui_text_box(chord, chip, 13, FONT_REGULAR, theme.text_dim, ALIGN_CENTER, 0);
    }
    header_draw((Rectangle){s.x, s.y, s.width, hh});
    if (!app_wide() && app.drawer_open) {
        ui_layer(1);
        ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.scrim);
        float dw = fminf(DP(300), ui.safe.width * 0.85f);
        Rectangle d = {0, 0, ui.safe.x + dw, ui.h};
        ui_rect(d, theme.surface);
        places_draw((Rectangle){ui.safe.x, ui.safe.y, dw, ui.safe.height});
        if (ui_live() && (ui.pressed || ui.rpressed) && !CheckCollisionPointRec(ui.mouse, d)) app.drawer_open = 0;
        ui_layer(0);
    }
    draw_menus();
    ops_draw_dialogs();
    ui_toast_draw(list.y + list.height);
    if (!modal) keys_handle();
    else if (ui_key(KEY_BACK, 0)) ui_menu_close();
    ops_poll_jobs();
    browser_poll();
}

int fr_app_main(int argc, char **argv)
{
    plat_init_window();
    theme_load();
    ui_input_init();
    fr_jobs_start(plat_wake);
    app.menu_more = ui_id("menu-more", 0), app.menu_row = ui_id("menu-row", 0), app.menu_bg = ui_id("menu-bg", 0);
    app.menu_selection = ui_id("menu-selection", 0);
    char *start = NULL;
    if (argc > 1) {
        char *real = realpath(argv[1], NULL);
        if (real) start = fr_uri_from_path(real);
        free(real);
    }
    if (!start) start = plat_home_uri();
    app.storage_ok = plat_storage_granted();
    ui.now = fr_now();
    places_refresh(1);
    browser_open(start, 0);
    free(start);
    float scale = 0;
    while (!app.quit && !WindowShouldClose()) {
        plat_frame();
        ui_begin_frame();
        if (ui.scale != scale) {
            if (scale) ui_fonts_reload(), ui_icons_clear();
            scale = ui.scale;
        }
        BeginDrawing();
        frame();
        EndDrawing();
        ui_end_frame();
        if (!ui.dirty && !ui_events_pending()) {
            double wait = 1.0;
            if (ui.redraw_at) wait = fmax(0, fmin(wait, ui.redraw_at - fr_now()));
            plat_wait(wait);
        }
    }
    fr_jobs_stop();
    CloseWindow();
    return 0;
}

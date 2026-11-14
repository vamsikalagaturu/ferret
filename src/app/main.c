#define _GNU_SOURCE
#include "app.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

fr_app app;

/* grows with the text size so names never clip */
float app_row_height(void)
{
    return ui.touch ? fmaxf(DP(64), ui_line_height(16) + ui_line_height(13) + DP(18)) : fmaxf(DP(38), ui_line_height(15) + DP(14));
}

static int drawing;
static struct {
    struct deferred {
        void (*fn)(const char *s, int n);
        char *s;
        int n;
    } *v;
    int n, cap;
} deferred;

void app_defer(void (*fn)(const char *s, int n), const char *s, int n)
{
    if (!drawing) {
        fn(s, n);
        return;
    }
    FR_PUSH(deferred, ((struct deferred){fn, s ? fr_strdup(s) : NULL, n}));
    ui.dirty = 1;
}

static void run_deferred(void)
{
    for (int i = 0; i < deferred.n; i++) {
        deferred.v[i].fn(deferred.v[i].s, deferred.v[i].n);
        free(deferred.v[i].s);
    }
    deferred.n = 0;
}
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
    if (ui_button(ui_id("paste-cancel", 0), (Rectangle){bx - DP(8) - cw, r.y + (h - bh) / 2, cw, bh}, "Cancel", 0)) ops_clip_clear();
    return h;
}

/* ---- menus ---- */

static void draw_menus(void)
{
    if (ui_menu_begin(app.menu_more)) {
        if (ui_menu_item("New folder", "folder-plus", -1)) ops_new_folder_start();
        if (ui_menu_item("New file", "file-plus", -1)) ops_new_start();
        if (ui_menu_item("Select all", "check-check", -1)) browser_select_all();
        if (clip.uris.n && ui_menu_item("Paste", "clipboard-paste", -1)) ops_paste();
        if (br.uri) {
            int fav = favorite_index(br.uri) >= 0;
            if (ui_menu_item(fav ? "Remove from favorites" : "Add to favorites", fav ? "star-off" : "star", -1)) favorite_toggle(br.uri);
        }
        ui_menu_separator();
        static const char *sorts[] = {"Sort by name", "Sort by size", "Sort by date", "Sort by kind", "Sort by extension"};
        for (int i = 0; i < 5; i++)
            if (ui_menu_item(sorts[i], NULL, br.sort == (fr_sort)i)) br.sort = (fr_sort)i, browser_rebuild_view();
        if (ui_menu_item("Reverse order", NULL, br.desc)) br.desc = !br.desc, browser_rebuild_view();
        if (ui_menu_item("Show hidden", NULL, br.show_hidden)) br.show_hidden = !br.show_hidden, browser_rebuild_view();
        ui_menu_separator();
        static const char *layouts[] = {"Details", "Columns", "Grid"};
        for (int i = 0; i < 3; i++)
            if (ui_menu_item(layouts[i], NULL, app.layout == (fr_layout)i)) view_set_layout((fr_layout)i);
        ui_menu_separator();
        char size[48];
        snprintf(size, sizeof size, "Bigger interface (now %d%%)", (int)lroundf(ui.user_scale * 100));
        if (ui_menu_item(size, "zoom-in", -1)) app_zoom(1);
        if (ui_menu_item("Smaller interface", "zoom-out", -1)) app_zoom(-1);
        if (ui.user_scale != 1 && ui_menu_item("Default interface size", NULL, -1)) app_zoom(0);
        char text[48];
        snprintf(text, sizeof text, "Bigger text (now %d%%)", (int)lroundf(ui.text_scale * 100));
        if (ui_menu_item(text, "a-large-small", -1)) app_text_zoom(1);
        if (ui_menu_item("Smaller text", NULL, -1)) app_text_zoom(-1);
        if (ui.text_scale != 1 && ui_menu_item("Default text size", NULL, -1)) app_text_zoom(0);
        if (ui_menu_item("Keyboard shortcuts", "keyboard", -1)) app.help_open = 1;
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
        if (br.nselected == 1 && it && it->e.is_dir) {
            char *uri = fr_uri_join(br.uri, it->e.name);
            int fav = favorite_index(uri) >= 0;
            if (ui_menu_item(fav ? "Remove from favorites" : "Add to favorites", fav ? "star-off" : "star", -1)) favorite_toggle(uri);
            free(uri);
        }
        ui_menu_separator();
        if (br.uri && fr_vfs_can_trash(br.uri) && ui_menu_item("Move to trash", "trash-2", -1)) ops_delete(0);
        if (ui_menu_item("Delete permanently", "trash-2", -1)) ops_delete(1);
        ui_menu_separator();
        if (ui_menu_item("Properties", "info", -1)) ops_properties();
        ui_menu_end();
    }
    if (ui_menu_begin(app.menu_bg)) {
        if (ui_menu_item("New folder", "folder-plus", -1)) ops_new_folder_start();
        if (ui_menu_item("New file", "file-plus", -1)) ops_new_start();
        if (clip.uris.n && ui_menu_item("Paste", "clipboard-paste", -1)) ops_paste();
        if (ui_menu_item("Select all", "check-check", -1)) browser_select_all();
        if (br.uri) {
            int fav = favorite_index(br.uri) >= 0;
            if (ui_menu_item(fav ? "Remove this folder from favorites" : "Add this folder to favorites", fav ? "star-off" : "star", -1))
                favorite_toggle(br.uri);
        }
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

void app_zoom(int dir)
{
    float s = ui.user_scale > 0 ? ui.user_scale : 1;
    s = dir > 0 ? s + 0.1f : dir < 0 ? s - 0.1f : 1;
    s = fmaxf((float)UI_SCALE_MIN, fminf((float)UI_SCALE_MAX, roundf(s * 10) / 10));
    ui.user_scale = s;
    ui_toast("Interface size %d%%", (int)lroundf(s * 100));
}

void app_text_zoom(int dir)
{
    float s = ui.text_scale > 0 ? ui.text_scale : 1;
    s = dir > 0 ? s + 0.1f : dir < 0 ? s - 0.1f : 1;
    s = fmaxf((float)TEXT_SCALE_MIN, fminf((float)TEXT_SCALE_MAX, roundf(s * 10) / 10));
    ui.text_scale = s;
    ui_toast("Text size %d%%", (int)lroundf(s * 100));
}

/* ---- the keyboard shortcuts ---- */

static const char *const shortcuts[][2] = {
    {"j / k, arrows", "down / up"},        {"h / l", "parent folder / open"},
    {"gg / G", "top / bottom"},            {"Ctrl+d / Ctrl+u", "half a page down / up"},
    {"Enter", "open"},                     {"Backspace, Alt+Left", "back"},
    {"Alt+Right", "forward"},              {"Ctrl+L", "type a location (Tab completes)"},
    {"/", "filter this folder"},           {"n / N", "next / previous row"},
    {"Space", "select and move down"},     {"v", "select a range"},
    {"Ctrl+A", "select all"},              {"Esc", "clear the selection, close"},
    {"yy / dd / p", "copy / cut / paste"}, {"yp / yP", "copy the item's / folder's full path"},
    {"r, i, F2", "rename here, before the extension"}, {"A", "rename here, at the end"},
    {"I", "rename here, at the start"},    {"c", "retype the name, keep the extension"},
    {"Enter / Esc", "keep / drop the new name"},       {"a", "new file, or folder when the name ends in /"},
    {"Ctrl+Shift+N", "new folder"},
    {"D, Delete", "move to the trash"},    {"X, Shift+Delete", "delete permanently"},
    {"Alt+Enter", "properties"},           {".", "show hidden files"},
    {"sn ss sm se", "sort by name, size, date, extension"}, {"S", "reverse the order"},
    {"Ctrl+1 / 2 / 3", "details, columns, grid"},          {"m", "add or remove this folder as a favorite"},
    {"g1 .. g9", "open favorite 1 to 9"},  {"gh, g~", "home"},
    {"Ctrl+= / Ctrl+- / Ctrl+0", "bigger / smaller / default interface"},
    {"Ctrl+Alt+= / - / 0", "bigger / smaller / default text"},
    {"F5, Ctrl+R", "refresh"},             {"?", "this list"},
    {"q, Ctrl+Q", "quit"},
};

static void draw_help(void)
{
    if (!app.help_open) return;
    ui_layer(1);
    ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.scrim);
    int n = (int)(sizeof shortcuts / sizeof *shortcuts);
    float lh = DP(26), pad = DP(24), kw = DP(200);
    float w = fminf(ui.safe.width - DP(32), DP(620)), h = fminf(ui.safe.height - DP(48), pad * 2 + DP(40) + n * lh);
    Rectangle card = {ui.safe.x + (ui.safe.width - w) / 2, ui.safe.y + (ui.safe.height - h) / 2, w, h};
    ui_round(card, 14, theme.surface_hi);
    ui_round_line(card, 14, theme.border);
    ui_text_box("Keyboard shortcuts", (Rectangle){card.x + pad, card.y + pad, w - 2 * pad, DP(28)}, 18, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    Rectangle body = {card.x + pad, card.y + pad + DP(40), w - 2 * pad, h - 2 * pad - DP(40)};
    static fr_scroll scroll;
    ui_scroll(ui_id("help-list", 0), &scroll, body, n * lh);
    ui_clip_begin(body);
    for (int i = 0; i < n; i++) {
        float y = body.y + i * lh - scroll.offset;
        ui_text_box(shortcuts[i][0], (Rectangle){body.x, y, kw - DP(12), lh}, 14, FONT_SEMIBOLD, theme.accent, ALIGN_LEFT, 0);
        ui_text_box(shortcuts[i][1], (Rectangle){body.x + kw, y, body.width - kw, lh}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    }
    ui_clip_end();
    int typed_q = 0;
    for (int i = 0; i < ui.nchars; i++)
        if (ui.chars[i] == '?') typed_q = 1;
    if (typed_q || ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0) || (ui.pressed && !CheckCollisionPointRec(ui.mouse, card)))
        app.help_open = 0, ui.nchars = 0;
    ui_layer(0);
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

/* draws and takes the widgets' input; 1 when the keys go to the list */
static int frame(void)
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
        return 0;
    }
    int modal = ops_dialog_open() || ui_menu_is_open() || app.help_open;
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
    draw_help();
    ui_toast_draw(list.y + list.height);
    if (modal && ui_key(KEY_BACK, 0)) ui_menu_close();
    return !modal;
}

/* what the frame asked for, then the keys and what changed elsewhere (jobs, disks, the folder, settings.toml) */
static void between_frames(int keys)
{
    run_deferred();
    if (keys) keys_handle();
    if (!app.storage_ok) return;
    if (app_wide() || app.drawer_open) places_refresh(0);
    ops_poll_jobs();
    disks_poll();
    browser_poll();
    settings_poll();
}

int fr_app_main(int argc, char **argv)
{
    int replay = argc > 2 && !strcmp(argv[1], "--replay");
    if (replay) {
        if (replay_load(argv[2], argc > 3 ? argv[3] : NULL)) return 1;
        argc -= 2, argv += 2;
    }
    plat_init_window();
    ui.user_scale = ui.text_scale = 1;
    settings_load();
    theme_load(settings.theme);
    if (!replay) ui_input_init();
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
        drawing = 1;
        int keys = frame();
        drawing = 0;
        if (replay) replay_capture();
        EndDrawing();
        ui_end_frame();
        between_frames(keys);
        if (replay && !replay_frame()) app.quit = 1;
        if (!ui.dirty && !ui_events_pending()) {
            double wait = 1.0;
            if (ui.redraw_at) wait = fmax(0, fmin(wait, ui.redraw_at - fr_now()));
            plat_wait(wait);
        }
    }
    fr_jobs_stop();
    plat_close_window();
    return replay ? replay_status() : 0;
}

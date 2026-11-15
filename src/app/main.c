#define _GNU_SOURCE
#include "app.h"
#include "core/watch.h"
#include "jobs/async.h"
#include "net/net.h"
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
        unsigned browser; /* the pane drawn when it was asked for: it runs there */
    } *v;
    int n, cap;
} deferred;

void app_defer(void (*fn)(const char *s, int n), const char *s, int n)
{
    if (!drawing) {
        fn(s, n);
        return;
    }
    FR_PUSH(deferred, ((struct deferred){fn, s ? fr_strdup(s) : NULL, n, br_cur->id}));
    ui.dirty = 1;
}

/* what was asked for in a tab closed since is dropped */
static void run_deferred(void)
{
    for (int i = 0; i < deferred.n; i++) {
        fr_browser *b = browser_by_id(deferred.v[i].browser);
        if (b) {
            browser_enter(b);
            deferred.v[i].fn(deferred.v[i].s, deferred.v[i].n);
            tabs_sync();
        }
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

/* Extract here, Extract to <name>/, Extract to..., Compress to <name>.zip, for what is chosen */
static void archive_items(void)
{
    int n = ops_archive_targets();
    if (n == 1) {
        if (ui_menu_item("Extract here", "package", -1)) ops_extract(0);
        fr_strs uris = browser_target_uris();
        char *name = fr_uri_name(uris.v[0]), *stem = fr_archive_stem(name), label[300];
        snprintf(label, sizeof label, "Extract to %s/", stem);
        free(name), free(stem);
        fr_strs_free(&uris);
        if (ui_menu_item(label, "folder-archive", -1)) ops_extract(1);
    } else if (n > 1 && ui_menu_item("Extract here, each into its own folder", "package", -1)) ops_extract(1);
    if (n && ui_menu_item("Extract to...", "folder-open", -1)) ops_extract_to();
    char *zip = br.uri && !fr_archive_is_uri(br.uri) ? ops_compress_name() : NULL;
    if (zip) {
        char label[300];
        snprintf(label, sizeof label, "Compress to %s", zip);
        if (ui_menu_item(label, "file-archive", -1)) ops_compress();
        free(zip);
    }
}

static void rename_items(void)
{
    if (br.nselected < 2) return;
    if (ui_menu_item("Rename as text", "list", -1)) ops_bulk_rename(1);
    if (ui_menu_item("Find and replace in names", "pencil", -1)) ops_bulk_rename(0);
}

static int link_here(void) { return !ui.touch && links_ready() && br.uri && fr_uri_local(br.uri) && !br.results; }

/* sort, layout and type filter: in the overflow menu on the desktop, in the "View and settings" sheet on phones */
static void view_items(void)
{
    if (ui_menu_heading("Sort by", br.desc ? "arrow-down" : "arrow-up", br.desc ? "Descending" : "Ascending"))
        br.desc = !br.desc, browser_rebuild_view();
    static const char *const sorts[] = {"Name", "Size", "Date", "Kind", "Type"};
    static const char *const sort_icons[] = {"arrow-down-a-z", "ruler", "calendar", "shapes", "file-type"};
    int s = ui_menu_segments(sorts, sort_icons, 5, (int)br.sort);
    if (s >= 0) br.sort = (fr_sort)s, browser_rebuild_view();
    ui_menu_heading("Layout", NULL, NULL);
    /* phones have no room for columns */
    static const char *const layouts[] = {"Details", "Columns", "Grid"}, *const touch_layouts[] = {"Details", "Grid"};
    static const char *const layout_icons[] = {"list", "columns-3", "layout-grid"}, *const touch_icons[] = {"list", "layout-grid"};
    if (ui.touch) {
        int l = ui_menu_segments(touch_layouts, touch_icons, 2, br.layout == LAYOUT_GRID ? 1 : 0);
        if (l >= 0) view_set_layout(l ? LAYOUT_GRID : LAYOUT_DETAILS);
    } else {
        int l = ui_menu_segments(layouts, layout_icons, 3, (int)br.layout);
        if (l >= 0) view_set_layout((fr_layout)l);
    }
    if (ui_menu_heading("Show only", br.kinds ? "x" : NULL, br.kinds ? "All" : NULL)) kinds_set(KF_ALL);
    static const char *const kinds[] = {"Images", "Video", "Audio", "Docs", "Archives", "Code"};
    static const char *const kind_icons[] = {"image", "video", "music", "file-text", "file-archive", "file-code"};
    int k = ui_menu_segments(kinds, kind_icons, 6, (int)br.kinds - 1);
    if (k >= 0) kinds_set((fr_kind_filter)(k + 1));
    ui_menu_separator();
}

/* phones follow the system's display and font size */
static void size_theme_items(void)
{
    if (!ui.touch) {
        char pct[16];
        snprintf(pct, sizeof pct, "%d%%", (int)lroundf(ui.user_scale * 100));
        int d = ui_menu_stepper("Interface", "zoom-in", pct);
        if (d) app_zoom(d == 2 ? 0 : d);
        snprintf(pct, sizeof pct, "%d%%", (int)lroundf(ui.text_scale * 100));
        d = ui_menu_stepper("Text", "a-large-small", pct);
        if (d) app_text_zoom(d == 2 ? 0 : d);
    }
    ui_menu_heading("Theme", NULL, NULL);
    static const char *const schemes[] = {"Dark", "Light", "System"}, *const scheme_icons[] = {"moon", "sun", "monitor"};
    int sc = ui_menu_segments(schemes, scheme_icons, 3, settings.scheme);
    if (sc >= 0) scheme_set(sc);
}

static void draw_menus(void)
{
    if (ui_menu_begin(app.menu_more)) {
        int fav = br.uri && favorite_index(br.uri) >= 0;
        static const char *labels[5], *icons[5];
        enum { A_FOLDER, A_FILE, A_ALL, A_FAV, A_PASTE };
        int ids[5], n = 0;
        labels[n] = "Folder", icons[n] = "folder-plus", ids[n++] = A_FOLDER;
        labels[n] = "File", icons[n] = "file-plus", ids[n++] = A_FILE;
        labels[n] = "Select all", icons[n] = "check-check", ids[n++] = A_ALL;
        labels[n] = fav ? "Unfavorite" : "Favorite", icons[n] = fav ? "star-off" : "star", ids[n++] = A_FAV;
        if (clip.uris.n) labels[n] = "Paste", icons[n] = "clipboard-paste", ids[n++] = A_PASTE;
        int a = ui_menu_segments(labels, icons, n, -1);
        if (a >= 0) {
            ui_menu_close();
            switch (ids[a]) {
            case A_FOLDER: ops_new_folder_start(); break;
            case A_FILE: ops_new_start(); break;
            case A_ALL: browser_select_all(); break;
            case A_FAV:
                if (br.uri) favorite_toggle(br.uri);
                break;
            default: ops_paste(); break;
            }
        }
        ui_menu_separator();
        view_items();
        if (!ui.touch) {
            tabs_menu_items();
            compare_menu_items();
            ui_menu_separator();
        }
        if (ui_menu_item("Show hidden", "eye", br.show_hidden)) br.show_hidden = !br.show_hidden, browser_rebuild_view();
        if (!ui.touch && ui_menu_item("Show git status", "list-checks", settings.git_status)) settings.git_status = !settings.git_status;
        if (link_here() && ui_menu_item("Paste as link", "link", -1)) links_paste(-1);
        if (ui_menu_item("Find duplicate files", "file-stack", -1)) dupes_start();
        if (br.dupes && br.view.n && ui_menu_item("Select extra copies", "list-checks", -1)) dupes_select_copies();
        if (ui_menu_item(sizes_running() ? "Stop calculating sizes" : "Show folder sizes", "ruler", -1)) sizes_measure(1);
        if (ui_menu_item("Bookmarks", "bookmark", -1)) bookmarks_panel_open();
        if (settings.recent.n > 1 && ui_menu_item("Recent folders", "history", -1)) recent_pick();
        if (ops_clip_history_count() && ui_menu_item("Paste from history", "clipboard-paste", -1)) ops_clip_history();
        if (undo_can(0) && ui_menu_item("Undo", "undo-2", -1)) undo_run(0);
        if (undo_can(1) && ui_menu_item("Redo", "redo-2", -1)) undo_run(1);
        ui_menu_separator();
        size_theme_items();
        if (!ui.touch) {
            ui_menu_heading("Tabs", NULL, NULL);
            static const char *const places[] = {"Top", "Bottom", "Left", "Right"},
                                     *const place_icons[] = {"arrow-up", "arrow-down", "arrow-left", "arrow-right"};
            int at = ui_menu_segments(places, place_icons, 4, settings.tabs_at);
            if (at >= 0) settings.tabs_at = at, ui.dirty = 1;
        }
        if (!ui.touch && ui_menu_item("Keyboard shortcuts", "keyboard", -1)) app.help_open = 1;
        if (br.uri && fr_uri_local(br.uri) && !plat_terminal(NULL) && ui_menu_item("Open terminal here", "terminal", -1))
            ops_terminal_here();
        ui_menu_separator();
        if (ui_menu_item("Check for updates", "download", -1)) update_check(1);
        if (ui_menu_item("Refresh", "refresh-cw", -1)) browser_reload();
        ui_menu_end();
    }
    place_menu_draw();
    if (ui_menu_begin(app.menu_row)) {
        fr_item *it = browser_item(br.cursor);
        if (br.nselected == 1 && it && ui_menu_item("Open", "external-link", -1)) ops_open_item(it);
        if (it && !it->e.is_dir && br.uri && ui_menu_item("Open with...", "app-window", -1)) ops_open_with();
        if (ui_menu_item("Copy", "copy", -1)) ops_copy(0);
        if (ui_menu_item("Cut", "scissors", -1)) ops_copy(1);
        if (app_split() && ui_menu_item("Copy to the other pane", "copy", -1)) ops_to_other_pane(0);
        if (app_split() && ui_menu_item("Move to the other pane", "scissors", -1)) ops_to_other_pane(1);
        if (!ui.touch && br.uri && fr_uri_local(br.uri) && ui_menu_item("Copy as link", "link", -1)) links_yank();
        if (it && it->e.is_dir && br.nselected <= 1 && ui_menu_item("Open in a new tab", "plus", -1)) view_open_in_tab(br.cursor);
        if (it && it->e.is_dir && ui_menu_item(sizes_running() ? "Stop calculating sizes" : "Show folder size", "ruler", -1)) sizes_measure(0);
        if (br.nselected == 1 && ui_menu_item("Rename", "pencil", -1)) ops_rename_start();
        rename_items();
        if (br.results && br.nselected == 1 && ui_menu_item("Show in folder", "folder-open", -1)) search_reveal();
        if (br.nselected == 1 && it && it->e.is_dir) {
            char *uri = fr_uri_join(br.uri, it->e.name);
            int fav = favorite_index(uri) >= 0;
            if (ui_menu_item(fav ? "Remove from favorites" : "Add to favorites", fav ? "star-off" : "star", -1)) favorite_toggle(uri);
            free(uri);
        }
        archive_items();
        ui_menu_separator();
        if (br.uri && fr_vfs_can_trash(br.uri) && ui_menu_item("Move to trash", "trash", -1)) ops_delete(0);
        if (ui_menu_item("Delete permanently", "trash-2", -1)) ops_delete(1);
        ui_menu_separator();
        if (ui_menu_item("Properties", "info", -1)) ops_properties();
        ui_menu_end();
    }
    if (ui_menu_begin(app.menu_bg)) {
        if (ui_menu_item("New folder", "folder-plus", -1)) ops_new_folder_start();
        if (ui_menu_item("New file", "file-plus", -1)) ops_new_start();
        if (clip.uris.n && ui_menu_item("Paste", "clipboard-paste", -1)) ops_paste();
        if (link_here()) {
            if (ui_menu_item("Paste as link", "link", -1)) links_paste(-1);
            if (ui_menu_item(settings.relative_links ? "Paste as absolute link" : "Paste as relative link", "link", -1))
                links_paste(!settings.relative_links);
        }
        if (ui_menu_item("Select all", "check-check", -1)) browser_select_all();
        if (br.uri) {
            int fav = favorite_index(br.uri) >= 0;
            if (ui_menu_item(fav ? "Remove this folder from favorites" : "Add this folder to favorites", fav ? "star-off" : "star", -1))
                favorite_toggle(br.uri);
        }
        if (ui_menu_item("Show hidden", "eye", br.show_hidden)) br.show_hidden = !br.show_hidden, browser_rebuild_view();
        if (br.uri && fr_uri_local(br.uri) && !plat_terminal(NULL) && ui_menu_item("Open terminal here", "terminal", -1))
            ops_terminal_here();
        if (ui_menu_item("Refresh", "refresh-cw", -1)) browser_reload();
        ui_menu_end();
    }
    trash_draw_menus();
    if (ui_menu_begin(app.menu_selection)) {
        if (ui.touch && ui_menu_item("Select all", "check-check", -1)) browser_select_all();
        if (br.nselected == 1 && ui_menu_item("Rename", "pencil", -1)) ops_rename_start();
        rename_items();
        if (br.results && br.nselected == 1 && ui_menu_item("Show in folder", "folder-open", -1)) search_reveal();
        if (br.nselected == 1 && br.uri && ui_menu_item("Open with...", "app-window", -1)) ops_open_with();
        archive_items();
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
    {"yy / dd / p", "copy / cut / paste"}, {"yh", "paste one of the last 10 copied or cut sets"},
    {"yp / yP", "copy the item's / folder's full path"},
    {"r, i, F2", "rename here, before the extension"}, {"A", "rename here, at the end"},
    {"I", "rename here, at the start"},    {"c", "retype the name, keep the extension"},
    {"Enter / Esc", "keep / drop the new name"},       {"a", "new file, or folder when the name ends in /"},
    {"Ctrl+Shift+N", "new folder"},
    {"R", "rename the targets as text, one name per line"}, {"E", "find and replace in the targets' names"},
    {"D, Delete", "move to the trash"},    {"X, Shift+Delete", "delete permanently"},
    {"u, Ctrl+Z", "undo the last file operation"},         {"U, Ctrl+Shift+Z", "redo"},
    {"ff", "search names below this folder (* and ?)"},   {"fg", "search the text inside files below"},
    {"gr", "show a search result in its folder"},          {"Esc, Backspace, h", "leave the search results"},
    {"Enter on an archive", "browse inside it (zip, tar, 7z, rar, iso)"},
    {"ze / zf", "extract here / into a folder named after it"},
    {"zt", "extract to a folder you type, each archive into its own"}, {"zc", "compress to <name>.zip"},
    {"fi fv fa", "show only images / video / audio (folders stay)"},
    {"fo fz fc", "show only documents / archives / code"}, {"fx, Esc", "show every kind again"},
    {"Alt+Enter", "properties"},           {".", "show hidden files"},
    {"O", "open with another app"},        {"T", "a terminal in this folder"},
    {"sn ss sm se", "sort by name, size, date, extension"}, {"S", "reverse the order"},
    {"Ctrl+1 / 2 / 3", "details, columns, grid"},          {"M", "add or remove this folder as a favorite"},
    {"g1 .. g9", "open favorite 1 to 9"},  {"gh, g~", "home"},
    {"g-", "recent folders (the last 20)"},
    {"m<key>", "bookmark this folder as key (a letter or digit)"}, {"'<key>", "go to that bookmark"},
    {"''", "back to the folder shown before"}, {"\"<key>", "remove that bookmark"},
    {"dc", "show folder sizes: the selected ones, else all (again: stop)"},
    {"fd", "find duplicate files (here and in subfolders)"}, {"fD", "in the duplicates: select the extra copies (one of each kept)"},
    {"Ctrl+= / Ctrl+- / Ctrl+0", "bigger / smaller / default interface"},
    {"Ctrl+Alt+= / - / 0", "bigger / smaller / default text"},
    {"F5, Ctrl+R", "refresh (Ctrl+R in the split view)"}, {"gd", "disks (the disk manager)"},
    {"?", "this list"},                    {"q, Ctrl+Q", "quit"},
    {NULL, "Tabs and the split view"},
    {"Ctrl+T, gn", "new tab of this folder"}, {"Ctrl+W, gc", "close the tab"},
    {"gt / gT", "next / previous tab"},    {"Ctrl+Tab, Ctrl+PgDn", "next tab"},
    {"Ctrl+Shift+Tab", "previous tab (also Ctrl+PgUp)"}, {"Alt+1 .. Alt+9", "tab 1 to 9"},
    {"middle click", "a folder: open it in a new tab; a tab: close it"},
    {"F3", "split view: two panes side by side, or back to one"},
    {"Tab", "the keys to the other pane"}, {"F5 / F6", "split view: copy / move to the other pane"},
    {"=", "compare the panes: tints and chips for only here, newer, older, differs"},
    {"+", "select what differs from the other pane"},
    {"> / <", "sync: copy what is missing or newer to the right / left pane (shows what first)"},
    {NULL, "Links, permissions, git"},
    {"yl", "take the targets to link to"},     {"P", "make links here to them (relative, see settings)"},
    {"Alt+Enter", "properties: permissions, owner and group of the selection"},
    {"M S U C", "git: modified, staged, untracked, conflict; ignored is dimmed"},
    {NULL, "Disks"},
    {"h / l, arrows", "previous / next partition"}, {"j / k, arrows", "next / previous disk"},
    {"0 / $", "first / last partition"},   {"gg / G", "first / last disk"},
    {"Enter, Space", "actions menu"},      {"o", "look inside: what uses the space"},
    {"m", "mount / unmount"},              {"r / f / t", "resize / format / set label"},
    {"n", "new partition in free space"},  {"dd", "delete partition"},
    {"w / E / P", "write a bootable image / eject / power off (USB)"},
    {"u", "undo the last queued change"},  {"a", "apply the queue (asks first)"},
    {"Tab / Shift+Tab", "in a form: next / previous filesystem"},
    {"drag right edge", "resize"},         {"drag free space", "new partition"},
    {"q, Esc", "back to the files"},
    {NULL, "Look inside"},
    {"j / k, gg / G", "move, top / bottom"}, {"Ctrl+d / Ctrl+u", "half a page down / up"},
    {"l, Enter / h, Backspace", "open folder / parent folder"}, {"r", "count again"},
    {"q, Esc", "back to the disks"},
};

static int help_matches(int i, const char *q)
{
    return !*q || (shortcuts[i][0] && (strcasestr(shortcuts[i][0], q) || strcasestr(shortcuts[i][1], q)));
}

/* typing filters the rows by key or description; a heading stays while one of its rows does. ? closes only with an
 * empty filter, Esc empties the filter first */
static void draw_help(void)
{
    static fr_text filter;
    static int was_open;
    static fr_scroll scroll;
    unsigned fid = ui_id("help-filter", 0);
    if (!app.help_open) {
        was_open = 0;
        return;
    }
    if (!was_open) text_set(&filter, ""), ui.focus = fid, scroll.offset = 0, was_open = 1;
    int typed_q = 0;
    for (int i = 0; !filter.len && i < ui.nchars; i++)
        if (ui.chars[i] == '?') typed_q = 1;
    if (typed_q) {
        app.help_open = 0, ui.nchars = 0, ui.focus = 0;
        return;
    }
    ui_layer(1);
    ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.scrim);
    int n = (int)(sizeof shortcuts / sizeof *shortcuts);
    float lh = DP(26), pad = DP(24), kw = DP(200), top = DP(40) + DP(36) + DP(12);
    float w = fminf(ui.safe.width - DP(32), DP(620)), h = fminf(ui.safe.height - DP(48), pad * 2 + top + n * lh);
    Rectangle card = {ui.safe.x + (ui.safe.width - w) / 2, ui.safe.y + (ui.safe.height - h) / 2, w, h};
    ui_round(card, 14, theme.surface_hi);
    ui_round_line(card, 14, theme.border);
    ui_text_box("Keyboard shortcuts", (Rectangle){card.x + pad, card.y + pad, w - 2 * pad, DP(28)}, 18, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    int res = ui_text_field(fid, &filter, (Rectangle){card.x + pad, card.y + pad + DP(40), w - 2 * pad, DP(36)}, "Search keys and actions");
    if (res & TEXT_CHANGED) scroll.offset = 0;
    const char *q = filter.buf;
    int show[sizeof shortcuts / sizeof *shortcuts], shown = 0;
    for (int i = 0; i < n; i++) {
        if (shortcuts[i][0]) {
            if (help_matches(i, q)) show[shown++] = i;
            continue;
        }
        int any = 0;
        for (int k = i + 1; k < n && shortcuts[k][0] && !any; k++) any = help_matches(k, q);
        if (any) show[shown++] = i;
    }
    Rectangle body = {card.x + pad, card.y + pad + top, w - 2 * pad, h - 2 * pad - top};
    ui_scroll(ui_id("help-list", 0), &scroll, body, shown * lh);
    ui_clip_begin(body);
    if (!shown) ui_text_box("No shortcut matches", (Rectangle){body.x, body.y, body.width, lh}, 14, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    for (int j = 0; j < shown; j++) {
        int i = show[j];
        float y = body.y + j * lh - scroll.offset;
        if (!shortcuts[i][0]) {
            ui_text_box(shortcuts[i][1], (Rectangle){body.x, y + DP(4), body.width, lh}, 13, FONT_SEMIBOLD, theme.text_dim, ALIGN_LEFT, 0);
            continue;
        }
        ui_text_box(shortcuts[i][0],(Rectangle){body.x, y, kw - DP(12), lh}, 14, FONT_SEMIBOLD, theme.accent, ALIGN_LEFT, 0);
        ui_text_box(shortcuts[i][1], (Rectangle){body.x + kw, y, body.width - kw, lh}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    }
    ui_clip_end();
    int esc = (res & TEXT_CANCEL) || (ui.focus != fid && (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)));
    if (esc && filter.len) text_set(&filter, ""), ui.focus = fid, scroll.offset = 0;
    else if (esc || (ui.pressed && !CheckCollisionPointRec(ui.mouse, card))) app.help_open = 0, ui.nchars = 0, ui.focus = 0;
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
    int modal = ops_dialog_open() || connect_dialog_open() || ui_menu_is_open() || app.help_open || dview_modal() || bookmarks_panel_is_open() ||
                compare_dialog_open();
    Rectangle s = ui.safe;
    if (app_wide()) {
        float sw = DP(240);
        places_draw((Rectangle){s.x, s.y, sw, s.height});
        ui_rect((Rectangle){s.x + sw - DP(1), s.y, DP(1), s.height}, theme.border);
        s.x += sw, s.width -= sw;
    }
    Rectangle list = s;
    if (dview_is_open()) {
        list.height -= ops_draw_jobs(list);
        dview_draw(list);
    } else {
        float th = tabbar_height(), tw = tabstrip_width();
        if (tw) {
            list.width -= tw;
            tabstrip_draw((Rectangle){list.x + list.width, list.y, tw, list.height});
        }
        if (th && settings.tabs_at == TABS_BOTTOM) {
            list.height -= th;
            tabbar_draw((Rectangle){list.x, list.y + list.height, list.width, th});
        } else if (th) {
            tabbar_draw((Rectangle){list.x, list.y, list.width, th});
            list.y += th, list.height -= th;
        }
        list.height -= ops_draw_jobs(list);
        list.height -= update_bar_draw(list);
        list.height -= draw_paste_bar(list);
        list.height -= compare_bar_draw(list);
        panes_draw(list);
        const char *chord = keys_pending();
        if (*chord) {
            float cw = ui_text_width(chord, 13, FONT_REGULAR) + DP(20), ch = DP(24);
            Rectangle chip = {list.x + list.width - cw - DP(16), list.y + list.height - ch - DP(12), cw, ch};
            ui_round(chip, 12, theme.surface_hi);
            ui_text_box(chord, chip, 13, FONT_REGULAR, theme.text_dim, ALIGN_CENTER, 0);
        }
    }
    /* the drawer slides: 180 ms, eased out opening and in closing */
    static float slide;
    float target = !app_wide() && app.drawer_open ? 1.0f : 0.0f;
    if (slide != target) {
        float step = (float)ui.dt / 0.18f;
        slide = target > slide ? fminf(target, slide + step) : fmaxf(target, slide - step);
        ui.dirty = 1;
    }
    if (!app_wide() && slide > 0) {
        float e = app.drawer_open ? 1 - (1 - slide) * (1 - slide) : slide * slide;
        ui_layer(1);
        ui_rect((Rectangle){0, 0, ui.w, ui.h}, ui_alpha(theme.scrim, theme.scrim.a / 255.0f * e));
        float dw = fminf(DP(300), ui.safe.width * 0.85f), off = -(ui.safe.x + dw) * (1 - e);
        Rectangle d = {off, 0, ui.safe.x + dw, ui.h};
        ui_rect(d, theme.surface);
        places_draw((Rectangle){ui.safe.x + off, ui.safe.y, dw, ui.safe.height});
        if (app.drawer_open && ui_live() && (ui.pressed || ui.rpressed) && !CheckCollisionPointRec(ui.mouse, d)) app.drawer_open = 0;
        ui_layer(0);
    }
    draw_menus();
    ops_draw_dialogs();
    compare_draw_dialog();
    connect_draw_dialogs();
    draw_help();
    bookmarks_draw(list);
    ui_toast_draw(list.y + list.height);
    if (modal && ui_key(KEY_BACK, 0)) ui_menu_close();
    return !modal;
}

/* what the frame asked for, then the keys and what changed elsewhere (jobs, disks, the folder, settings.toml) */
static void between_frames(int keys)
{
    run_deferred();
    drop_poll();
    thumbs_poll();
    if (keys) keys_handle();
    if (!app.storage_ok) return;
    fr_async_poll();
    if (app_wide() || app.drawer_open) places_refresh(0);
    ops_poll_jobs();
    sizes_poll();
    search_poll();
    dupes_poll();
    preview_poll();
    disks_poll();
    dview_poll();
    browser_poll();
    compare_poll();
    git_poll();
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
    panes_init();
    settings_load();
    theme_apply();
    if (!replay) ui_input_init();
    fr_jobs_start(plat_wake);
    fr_async_start(plat_wake);
    fetch_start();
    fr_watch_start(plat_wake);
    static const fr_secret_store keystore = {plat_secret_load, plat_secret_save, plat_secret_forget};
    if (plat_secret_available()) fr_creds_set_store(&keystore);
    thumbs_start();
    if (!replay) update_check(0);
    app.menu_more = ui_id("menu-more", 0), app.menu_row = ui_id("menu-row", 0), app.menu_bg = ui_id("menu-bg", 0);
    app.menu_selection = ui_id("menu-selection", 0);
    char *start = NULL;
    if (argc > 1) {
        char *real = realpath(argv[1], NULL);
        if (real) start = fr_uri_from_path(real);
        free(real);
    }
    app.storage_ok = plat_storage_granted();
    fr_strs restored = {0};
    int current = -1;
    if (!start && app.storage_ok) restored = settings_start_tabs(&current);
    if (!start && app.storage_ok) start = settings_start_uri();
    if (!start) start = plat_home_uri();
    ui.now = fr_now();
    places_refresh(1);
    if (restored.n) {
        browser_open(restored.v[0], 0);
        for (int i = 1; i < restored.n; i++) tab_new(restored.v[i], 1);
        tab_select(current >= 0 ? current : 0);
    } else browser_open(start, 0);
    fr_strs_free(&restored);
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
    settings_flush();
    dview_shutdown();
    fr_watch_stop();
    fr_jobs_stop();
    thumbs_stop();
    search_stop();
    fr_conn_close_all();
    plat_close_window();
    return replay ? replay_status() : 0;
}

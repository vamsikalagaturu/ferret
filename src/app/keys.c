#include "app.h"
#include <stdlib.h>
#include <string.h>

void filter_start(int first_char)
{
    br.filtering = 1;
    char first[5] = "";
    if (first_char) first[fr_utf8_encode(first_char, first)] = 0;
    text_set(&br.filter, first);
    ui.focus = ui_id("filter", 0);
    browser_rebuild_view();
}

void filter_stop(void)
{
    br.filtering = 0;
    ui.focus = 0;
    browser_rebuild_view();
}

void move_cursor(int to, int extend)
{
    if (!br.view.n) return;
    if (to < 0) to = 0;
    if (to >= br.view.n) to = br.view.n - 1;
    br.cursor = to;
    br.keyboard = 1;
    if (extend) {
        browser_clear_selection();
        int a = br.anchor < to ? br.anchor : to, b = br.anchor < to ? to : br.anchor;
        for (int i = a; i <= b; i++) browser_set_selected(br.view.v[i], 1);
    } else br.anchor = to;
    view_reveal(to);
    ui.dirty = 1;
}

int step_back(int quit_at_end)
{
    if (app.drawer_open) return app.drawer_open = 0, 1;
    if (br.nselected) return browser_clear_selection(), 1;
    if (br.filtering) return filter_stop(), 1;
    if (!quit_at_end) return 0;
    if (br.back.n) return browser_back(), 1;
    char *parent = br.uri ? fr_uri_parent(br.uri) : NULL;
    int p = br.uri ? place_of(br.uri) : -1;
    if (parent && !(p >= 0 && !strcmp(app.places.v[p].uri, br.uri))) {
        browser_open(parent, 0);
        free(parent);
        return 1;
    }
    free(parent);
    app.quit = 1;
    return 1;
}

void keys_handle(void)
{
    if (ui_key(KEY_BACK, 0)) step_back(1);
    if (ui_key(KEY_ESCAPE, 0)) step_back(0);
    if (ui.focus) return;
    int page = (int)(br.scroll.view / app_row_height()) - 1;
    if (page < 1) page = 1;
    for (int shift = 0; shift <= 1; shift++) {
        int m = shift ? MOD_SHIFT : 0;
        if (ui_key(KEY_DOWN, m)) move_cursor(br.cursor + 1, shift);
        if (ui_key(KEY_UP, m)) move_cursor(br.cursor - 1, shift);
        if (ui_key(KEY_PAGE_DOWN, m)) move_cursor(br.cursor + page, shift);
        if (ui_key(KEY_PAGE_UP, m)) move_cursor(br.cursor - page, shift);
        if (ui_key(KEY_HOME, m)) move_cursor(0, shift);
        if (ui_key(KEY_END, m)) move_cursor(br.view.n - 1, shift);
    }
    if (ui_key(KEY_ENTER, 0) || ui_key(KEY_KP_ENTER, 0)) {
        fr_item *it = browser_item(br.cursor);
        if (it) ops_open_item(it);
    }
    if (ui_key(KEY_BACKSPACE, 0) || ui_key(KEY_LEFT, MOD_ALT)) browser_back();
    if (ui_key(KEY_RIGHT, MOD_ALT)) browser_forward();
    if (ui_key(KEY_UP, MOD_ALT)) browser_up();
    if (ui_key(KEY_A, MOD_CTRL)) browser_select_all();
    if (ui_key(KEY_C, MOD_CTRL)) ops_copy(0);
    if (ui_key(KEY_X, MOD_CTRL)) ops_copy(1);
    if (ui_key(KEY_V, MOD_CTRL)) ops_paste();
    if (ui_key(KEY_DELETE, 0)) ops_delete(0);
    if (ui_key(KEY_DELETE, MOD_SHIFT)) ops_delete(1);
    if (ui_key(KEY_F2, 0)) ops_rename_start();
    if (ui_key(KEY_N, MOD_CTRL | MOD_SHIFT)) ops_new_folder_start();
    if (ui_key(KEY_H, MOD_CTRL)) br.show_hidden = !br.show_hidden, browser_rebuild_view();
    if (ui_key(KEY_F5, 0) || ui_key(KEY_R, MOD_CTRL)) browser_reload();
    if (ui_key(KEY_F, MOD_CTRL)) filter_start(0);
    if (ui_key(KEY_ENTER, MOD_ALT)) ops_properties();
    if (ui_key(KEY_Q, MOD_CTRL)) app.quit = 1;
    if (!ui.touch && ui.nchars && ui.chars[0] > ' ' && ui.chars[0] != 127) filter_start(ui.chars[0]);
}

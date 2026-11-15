#include "app.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define K(prefix, c) ((prefix) << 8 | (c))

static char pending[2]; /* the chord prefix typed so far */
static int visual;      /* cursor moves extend the selection from the anchor */

static struct {
    int on;
    int seen;   /* a layout drew the field since the last keys_handle */
    int result; /* what the field returned since then */
    int creating; /* naming a new file or folder in the placeholder row; else renaming uri */
    char *uri;
    fr_text text;
} insert;

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
        int from = br.anchor >= 0 && br.anchor < br.view.n ? br.anchor : to;
        int a = from < to ? from : to, b = from < to ? to : from;
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
    if (br.results) return browser_results_leave(), 1;
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

const char *keys_pending(void) { return pending; }

int keys_mode(void) { return insert.on ? MODE_INSERT : visual ? MODE_VISUAL : MODE_NORMAL; }

static unsigned insert_id(void) { return ui_id("insert", 0); }

/* how: 'i' caret before the extension, 'A' at the end, 'I' at the start, 'c' the stem cleared */
static void insert_start(int how)
{
    fr_item *it = browser_item(br.cursor);
    if (!it || !br.uri) return;
    visual = 0;
    const char *name = strrchr(it->e.name, '/') ? strrchr(it->e.name, '/') + 1 : it->e.name; /* a search result's path */
    int stem = (int)fr_stem_len(name, it->e.is_dir);
    text_set(&insert.text, how == 'c' ? name + stem : name);
    int at = how == 'I' || how == 'c' ? 0 : how == 'i' ? stem : insert.text.len;
    text_select(&insert.text, at, at);
    free(insert.uri);
    insert.uri = fr_uri_join(br.uri, it->e.name);
    insert.on = insert.seen = 1;
    insert.creating = 0;
    ui.focus = insert_id();
    view_reveal(br.cursor);
    ui.dirty = 1;
}

void keys_new_start(const char *typed)
{
    if (!br.uri) return;
    visual = 0;
    browser_clear_selection();
    browser_add_placeholder();
    text_set(&insert.text, typed);
    size_t n = strlen(typed);
    text_select(&insert.text, 0, (int)(n && typed[n - 1] == '/' ? n - 1 : n));
    free(insert.uri);
    insert.uri = NULL;
    insert.on = insert.seen = insert.creating = 1;
    ui.focus = insert_id();
    view_reveal(br.cursor);
    ui.dirty = 1;
}

static void insert_stop(void)
{
    insert.on = 0;
    if (insert.creating) browser_drop_placeholder();
    insert.creating = 0;
    free(insert.uri);
    insert.uri = NULL;
    if (ui.focus == insert_id()) ui.focus = 0;
    ui.dirty = 1;
}

static void insert_commit(void)
{
    if (insert.creating) {
        if (!ops_create(insert.text.buf)) insert.creating = 0, insert_stop();
        return;
    }
    char *old = fr_uri_name(insert.uri);
    int same = !strcmp(old, insert.text.buf);
    free(old);
    if (same) {
        insert_stop();
        return;
    }
    if (!fr_name_valid(insert.text.buf)) {
        ui_toast("A name cannot be empty, \".\", \"..\" or contain \"/\"");
        return;
    }
    char *parent = fr_uri_parent(insert.uri), *to = fr_uri_join(parent, insert.text.buf);
    int rc = ops_rename(insert.uri, to);
    free(parent), free(to);
    if (rc == -EEXIST) ui_toast("\"%s\" already exists here", insert.text.buf);
    else if (rc) ui_toast("Could not rename: %s", fr_vfs_error(rc));
    else {
        free(br.focus_name);
        br.focus_name = fr_strdup(insert.text.buf);
        browser_reload();
        insert_stop();
    }
}

int keys_insert_field(int v, Rectangle r)
{
    if (!insert.on || v != br.cursor) return 0;
    fr_item *it = browser_item(v);
    if (insert.creating) {
        if (!it || !it->placeholder) return 0;
        int folder = insert.text.len && insert.text.buf[insert.text.len - 1] == '/';
        it->e.is_dir = folder;
        it->kind = folder ? KIND_FOLDER : insert.text.len ? kind_of(insert.text.buf, 0) : KIND_OTHER;
    } else {
        char *uri = it ? fr_uri_join(br.uri, it->e.name) : NULL;
        int same = uri && insert.uri && !strcmp(uri, insert.uri);
        free(uri);
        if (!same) return 0;
    }
    insert.seen = 1;
    insert.result |= ui_text_field(insert_id(), &insert.text, r, NULL);
    return 1;
}

/* after the layouts drew: committing reloads the folder, which must not happen while its rows are drawn */
static void insert_poll(void)
{
    int rc = insert.result, seen = insert.seen;
    insert.result = insert.seen = 0;
    if (!insert.on) return;
    if (rc & TEXT_CANCEL || !seen || ui.focus != insert_id()) insert_stop();
    else if (rc & TEXT_SUBMIT) insert_commit();
}

static void set_pending(int c)
{
    pending[0] = (char)c;
    ui.dirty = 1;
}

static void go(int to) { move_cursor(to, visual); }

static void wrap(int by)
{
    if (br.view.n) go(((br.cursor + by) % br.view.n + br.view.n) % br.view.n);
}

static void open_cursor(void)
{
    fr_item *it = browser_item(br.cursor);
    if (it) ops_open_item(it);
}

static void go_home(void)
{
    char *home = plat_home_uri();
    if (home) browser_open(home, 1);
    free(home);
}

static void go_favorite(int n)
{
    if (n <= settings.favorites.n) browser_open(settings.favorites.v[n - 1], 1);
    else ui_toast("No favorite %d", n);
}

static void sort_by(fr_sort s)
{
    br.sort = s;
    browser_rebuild_view();
}

static void toggle_hidden(void)
{
    br.show_hidden = !br.show_hidden;
    browser_rebuild_view();
}

static void toggle_selected(void)
{
    fr_item *it = browser_item(br.cursor);
    if (!it) return;
    visual = 0;
    browser_set_selected(br.view.v[br.cursor], !it->selected);
    move_cursor(br.cursor + 1, 0);
}

static void toggle_visual(void)
{
    if (visual || !br.view.n) {
        visual = 0;
        return;
    }
    visual = 1;
    br.anchor = br.cursor;
    go(br.cursor);
}

static void clip_take(int cut)
{
    visual = 0;
    ops_copy(cut);
}

/* the current folder's path (here), else the selected items' or the cursor row's, one per line */
static void copy_path(int here)
{
    fr_strs uris = {0};
    if (here) {
        FR_PUSH(uris, fr_strdup(br.uri));
        app.path_flash_at = ui.now;
    } else {
        browser_flash_targets();
        uris = browser_target_uris();
    }
    size_t len = 1;
    for (int i = 0; i < uris.n; i++) len += strlen(uris.v[i]) + 1;
    char *text = fr_realloc(NULL, len), *o = text;
    for (int i = 0; i < uris.n; i++) {
        const char *p = fr_uri_local(uris.v[i]);
        o += sprintf(o, "%s%s", i ? "\n" : "", p ? p : uris.v[i]);
    }
    *o = 0;
    if (uris.n) {
        plat_set_clipboard(text);
        if (uris.n == 1) ui_toast("Copied %s", text);
        else ui_toast("Copied %d paths", uris.n);
    }
    free(text);
    fr_strs_free(&uris);
}

/* a pressed key that is no typed character (Ctrl/Alt combos, arrows, F-keys) cancels a pending chord */
static int command_key(void)
{
    for (int i = 0; i < ui.nkeys; i++) {
        int k = ui.keys[i].key;
        if (ui.keys[i].used || (k >= KEY_LEFT_SHIFT && k <= KEY_KB_MENU)) continue;
        if (ui.keys[i].mods & ~MOD_SHIFT || (k >= KEY_ESCAPE && !(k >= KEY_KP_0 && k <= KEY_KP_EQUAL))) return 1;
    }
    return 0;
}

static void command(int c)
{
    int p = pending[0];
    if (c < ' ') return;
    if (!p && strchr("gsydzf", c)) {
        set_pending(c);
        return;
    }
    if (p) set_pending(0);
    if (c > '~') return;
    if (p == 'g' && c >= '1' && c <= '9') {
        go_favorite(c - '0');
        return;
    }
    int cols = view_grid_columns();
    switch (K(p, c)) {
    case K(0, 'j'): go(br.cursor + cols); break;
    case K(0, 'k'): go(br.cursor - cols); break;
    case K('g', 'g'): go(0); break;
    case K(0, 'G'): go(br.view.n - 1); break;
    case K(0, 'h'): browser_up(); break;
    case K(0, 'l'): open_cursor(); break;
    case K(0, '/'): filter_start(0); break;
    case K(0, 'n'): wrap(1); break;
    case K(0, 'N'): wrap(-1); break;
    case K(0, ' '): toggle_selected(); break;
    case K(0, 'v'): toggle_visual(); break;
    case K('y', 'y'): clip_take(0); break;
    case K(0, '?'): app.help_open = 1; break;
    case K('y', 'p'): copy_path(0); break;
    case K('y', 'P'): copy_path(1); break;
    case K('d', 'd'): clip_take(1); break;
    case K(0, 'p'): ops_paste(); break;
    case K(0, 'D'): ops_delete(0); break;
    case K(0, 'X'): ops_delete(1); break;
    case K(0, 'a'): keys_new_start(""); break;
    case K(0, 'r'): insert_start('i'); break;
    case K(0, 'i'):
    case K(0, 'A'):
    case K(0, 'I'):
    case K(0, 'c'): insert_start(c); break;
    case K(0, '.'): toggle_hidden(); break;
    case K(0, 'q'): app.quit = 1; break;
    case K('g', 'h'):
    case K('g', '~'): go_home(); break;
    case K(0, 'm'):
        if (br.uri) favorite_toggle(br.uri);
        break;
    case K('s', 'n'): sort_by(SORT_NAME); break;
    case K('s', 's'): sort_by(SORT_SIZE); break;
    case K('s', 'm'): sort_by(SORT_DATE); break;
    case K('s', 'e'): sort_by(SORT_EXT); break;
    case K('z', 'e'): ops_extract(0); break;
    case K('z', 'f'): ops_extract(1); break;
    case K('z', 'c'): ops_compress(); break;
    case K('f', 'f'): search_prompt(0); break;
    case K('f', 'g'): search_prompt(1); break;
    case K('g', 'r'): search_reveal(); break;
    case K(0, 'u'): undo_run(0); break;
    case K(0, 'U'): undo_run(1); break;
    case K(0, 'R'): ops_bulk_rename(1); break;
    case K(0, 'E'): ops_bulk_rename(0); break;
    case K(0, 'S'):
        br.desc = !br.desc;
        browser_rebuild_view();
        break;
    default: break;
    }
}

void keys_handle(void)
{
    insert_poll();
    if (visual && !br.nselected) visual = 0;
    if (ui_key(KEY_BACK, 0)) step_back(1);
    if (ui_key(KEY_ESCAPE, 0)) {
        if (pending[0]) set_pending(0);
        else visual = 0, step_back(0);
    }
    if (ui.focus) return;
    if (pending[0] && command_key()) set_pending(0);
    int cols = view_grid_columns(), page = (int)(br.scroll.view / app_row_height()) - 1;
    if (page < 1) page = 1;
    for (int shift = 0; shift <= 1; shift++) {
        int m = shift ? MOD_SHIFT : 0, ext = shift || visual;
        if (ui_key(KEY_DOWN, m)) move_cursor(br.cursor + cols, ext);
        if (ui_key(KEY_UP, m)) move_cursor(br.cursor - cols, ext);
        if (app.layout == LAYOUT_GRID && ui_key(KEY_RIGHT, m)) move_cursor(br.cursor + 1, ext);
        if (app.layout == LAYOUT_GRID && ui_key(KEY_LEFT, m)) move_cursor(br.cursor - 1, ext);
        if (app.layout == LAYOUT_COLUMNS && !m && ui_key(KEY_LEFT, 0)) browser_up();
        if (app.layout == LAYOUT_COLUMNS && !m && ui_key(KEY_RIGHT, 0)) {
            fr_item *it = browser_item(br.cursor);
            if (it && it->e.is_dir) ops_open_item(it);
        }
        if (ui_key(KEY_PAGE_DOWN, m)) move_cursor(br.cursor + page, ext);
        if (ui_key(KEY_PAGE_UP, m)) move_cursor(br.cursor - page, ext);
        if (ui_key(KEY_HOME, m)) move_cursor(0, ext);
        if (ui_key(KEY_END, m)) move_cursor(br.view.n - 1, ext);
    }
    if (ui_key(KEY_D, MOD_CTRL)) go(br.cursor + (page + 1) / 2);
    if (ui_key(KEY_U, MOD_CTRL)) go(br.cursor - (page + 1) / 2);
    if (ui_key(KEY_ENTER, 0) || ui_key(KEY_KP_ENTER, 0)) open_cursor();
    if (ui_key(KEY_BACKSPACE, 0) || ui_key(KEY_LEFT, MOD_ALT)) browser_back();
    if (ui_key(KEY_RIGHT, MOD_ALT)) browser_forward();
    if (ui_key(KEY_UP, MOD_ALT)) browser_up();
    if (ui_key(KEY_A, MOD_CTRL)) browser_select_all();
    if (ui_key(KEY_C, MOD_CTRL)) clip_take(0);
    if (ui_key(KEY_X, MOD_CTRL)) clip_take(1);
    if (ui_key(KEY_V, MOD_CTRL)) ops_paste();
    if (ui_key(KEY_DELETE, 0)) ops_delete(0);
    if (ui_key(KEY_DELETE, MOD_SHIFT)) ops_delete(1);
    if (ui_key(KEY_F2, 0)) {
        if (ui.touch) ops_rename_start();
        else insert_start('i');
    }
    if (ui_key(KEY_N, MOD_CTRL | MOD_SHIFT)) keys_new_start("New folder/");
    if (ui_key(KEY_Z, MOD_CTRL)) undo_run(0);
    if (ui_key(KEY_Z, MOD_CTRL | MOD_SHIFT)) undo_run(1);
    if (ui_key(KEY_H, MOD_CTRL)) toggle_hidden();
    if (ui_key(KEY_F5, 0) || ui_key(KEY_R, MOD_CTRL)) browser_reload();
    if (ui_key(KEY_F, MOD_CTRL)) filter_start(0);
    if (ui_key(KEY_ENTER, MOD_ALT)) ops_properties();
    if (ui_key(KEY_Q, MOD_CTRL)) app.quit = 1;
    if (ui_key(KEY_ONE, MOD_CTRL)) view_set_layout(LAYOUT_DETAILS);
    if (ui_key(KEY_TWO, MOD_CTRL)) view_set_layout(LAYOUT_COLUMNS);
    if (ui_key(KEY_THREE, MOD_CTRL)) view_set_layout(LAYOUT_GRID);
    if (ui_key(KEY_L, MOD_CTRL)) path_edit_start();
    if (ui_key(KEY_EQUAL, MOD_CTRL) || ui_key(KEY_EQUAL, MOD_CTRL | MOD_SHIFT) || ui_key(KEY_KP_ADD, MOD_CTRL)) app_zoom(1);
    if (ui_key(KEY_MINUS, MOD_CTRL) || ui_key(KEY_KP_SUBTRACT, MOD_CTRL)) app_zoom(-1);
    if (ui_key(KEY_ZERO, MOD_CTRL) || ui_key(KEY_KP_0, MOD_CTRL)) app_zoom(0);
    int ca = MOD_CTRL | MOD_ALT;
    if (ui_key(KEY_EQUAL, ca) || ui_key(KEY_EQUAL, ca | MOD_SHIFT) || ui_key(KEY_KP_ADD, ca)) app_text_zoom(1);
    if (ui_key(KEY_MINUS, ca) || ui_key(KEY_KP_SUBTRACT, ca)) app_text_zoom(-1);
    if (ui_key(KEY_ZERO, ca) || ui_key(KEY_KP_0, ca)) app_text_zoom(0);
    for (int i = 0; i < ui.nchars && !ui.focus && !ops_dialog_open() && !app.quit; i++) command(ui.chars[i]);
    ui.nchars = 0;
}

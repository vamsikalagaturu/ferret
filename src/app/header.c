#include "app.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float header_height(void) { return DP(ui.touch ? 56 : 48); }

static void draw_breadcrumb(Rectangle r)
{
    if (!br.uri) return;
    struct {
        char *label, *uri;
    } segs[64];
    int n = 0;
    const char *rest = fr_uri_path(br.uri), *end_of_uri = br.uri + strlen(br.uri);
    if (rest < br.uri || rest > end_of_uri) rest = end_of_uri;
    if (fr_uri_local(br.uri)) segs[n].label = fr_strdup("/");
    else segs[n].label = fr_strndup(br.uri, (size_t)(rest - br.uri));
    segs[n++].uri = fr_strndup(br.uri, (size_t)(rest - br.uri) + 1);
    while (*rest && n < 64) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/');
        if (!end) end = rest + strlen(rest);
        segs[n].label = fr_strndup(rest, (size_t)(end - rest));
        segs[n++].uri = fr_strndup(br.uri, (size_t)(end - br.uri));
        rest = end;
    }
    float sep = DP(20), total = 0, widths[64];
    for (int i = 0; i < n; i++) {
        widths[i] = ui_text_width(segs[i].label, 15, i == n - 1 ? FONT_SEMIBOLD : FONT_REGULAR) + DP(12);
        total += widths[i] + (i ? sep : 0);
    }
    int first = 0;
    float dots = ui_text_width("...", 15, FONT_REGULAR) + DP(12) + sep;
    while (first < n - 1 && total + (first ? dots : 0) > r.width) total -= widths[first] + sep, first++;
    float x = r.x;
    ui_clip_begin(r);
    if (first) {
        ui_text_box("...", (Rectangle){x, r.y, dots - sep, r.height}, 15, FONT_REGULAR, theme.text_faint, ALIGN_CENTER, 0);
        x += dots - sep;
    }
    for (int i = first; i < n; i++) {
        if (i > first || first) {
            ui_icon("chevron-right", (Rectangle){x, r.y, sep, r.height}, 14, theme.text_faint);
            x += sep;
        }
        int last = i == n - 1;
        float w = fminf(widths[i], r.x + r.width - x);
        Rectangle seg = {x, r.y + DP(8), w, r.height - DP(16)};
        int state;
        if (ui_clickable(ui_id("crumb", i), seg, &state) && !last) browser_open(segs[i].uri, 1);
        if (state && !last) ui_round(seg, 6, state == 2 ? theme.pressed : theme.hover);
        ui_text_box(segs[i].label, (Rectangle){x + DP(6), r.y, w - DP(12), r.height}, 15, last ? FONT_SEMIBOLD : FONT_REGULAR,
                    last ? theme.text : theme.text_dim, ALIGN_LEFT, 0);
        x += w;
    }
    ui_clip_end();
    for (int i = 0; i < n; i++) free(segs[i].label), free(segs[i].uri);
    Rectangle rest_r = {x, r.y, r.x + r.width - x, r.height};
    if (rest_r.width > 0 && ui_clickable(ui_id("crumb-rest", 0), rest_r, NULL)) path_edit_start();
}

/* ---- typing a location ---- */

static int editing;
static fr_text path_field;

void path_edit_start(void)
{
    if (!br.uri) return;
    const char *local = fr_uri_local(br.uri);
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    char *shown;
    if (local && hl > 1 && !strncmp(local, home, hl) && (local[hl] == '/' || !local[hl])) shown = fr_format("~%s/", local + hl);
    else if (local) shown = fr_format("%s%s", local, strcmp(local, "/") ? "/" : "");
    else shown = fr_strdup(br.uri);
    text_set(&path_field, shown);
    free(shown);
    editing = 1;
    ui.focus = ui_id("path", 0);
    ui.dirty = 1;
}

static void path_edit_stop(void)
{
    editing = 0;
    ui.focus = 0;
    ui.nchars = 0;
    ui.dirty = 1;
}

/* "~/x" -> "/home/u/x"; NULL for a relative path */
static char *expand(const char *s)
{
    const char *home = getenv("HOME");
    if (s[0] == '~' && (s[1] == '/' || !s[1]) && home) return fr_format("%s%s", home, s + 1);
    return *s == '/' ? fr_strdup(s) : NULL;
}

/* Tab: complete the last segment to the longest prefix the folders there share */
static void complete(void)
{
    if (strstr(path_field.buf, "://")) return;
    char *full = expand(path_field.buf);
    if (!full) return;
    char *slash = strrchr(full, '/');
    char *dir = fr_strndup(full, (size_t)(slash - full + 1)), *prefix = slash + 1;
    char *uri = fr_uri_from_path(dir);
    fr_entries es = {0};
    char *best = NULL;
    int matches = 0;
    if (!fr_vfs_list(uri, &es))
        for (int i = 0; i < es.n; i++) {
            if (!es.v[i].is_dir || strncmp(es.v[i].name, prefix, strlen(prefix))) continue;
            if (es.v[i].hidden && !*prefix) continue;
            if (!best) best = fr_strdup(es.v[i].name);
            else {
                size_t k = 0;
                while (best[k] && best[k] == es.v[i].name[k]) k++;
                best[k] = 0;
            }
            matches++;
        }
    if (best && strlen(best) >= strlen(prefix)) {
        size_t keep = strlen(path_field.buf) - strlen(prefix);
        char *done = fr_format("%.*s%s%s", (int)keep, path_field.buf, best, matches == 1 ? "/" : "");
        text_set(&path_field, done);
        free(done);
    }
    free(best);
    fr_entries_free(&es);
    free(uri), free(dir), free(full);
}

static void path_go(void)
{
    char *target = NULL;
    if (strstr(path_field.buf, "://")) target = fr_strdup(path_field.buf);
    else {
        char *full = expand(path_field.buf);
        if (!full) {
            ui_toast("Type a full path: starting with / or ~");
            return;
        }
        size_t n = strlen(full);
        while (n > 1 && full[n - 1] == '/') full[--n] = 0;
        target = fr_uri_from_path(full);
        free(full);
    }
    fr_entry e = {0};
    int rc = fr_vfs_stat(target, &e);
    if (rc) ui_toast("%s: %s", path_field.buf, fr_vfs_error(rc));
    else if (e.is_dir) {
        path_edit_stop();
        browser_open(target, 1);
    } else {
        char *parent = fr_uri_parent(target);
        path_edit_stop();
        if (parent) {
            br.focus_name = fr_strdup(e.name);
            browser_open(parent, 1);
        }
        free(parent);
    }
    fr_entry_free(&e);
    free(target);
}

void header_draw(Rectangle r)
{
    ui_rect(r, theme.bg);
    float b = DP(ui.touch ? 48 : 36), x = r.x + DP(4), right = r.x + r.width - DP(4), cy = r.y + (r.height - b) / 2;
    Rectangle btn = {x, cy, b, b};
    if (br.nselected) {
        ui_rect(r, ui_mix(theme.bg, theme.accent, 0.12f));
        if (ui_icon_button(ui_id("sel-clear", 0), btn, "x", theme.text)) browser_clear_selection();
        x += b + DP(4);
        static const char *icons[] = {"ellipsis-vertical", "trash-2", "scissors", "copy", "check-check"};
        int clicked = -1;
        for (int i = 0; i < 5; i++) {
            right -= b;
            if (ui_icon_button(ui_id("sel-action", i), (Rectangle){right, cy, b, b}, icons[i], theme.text)) clicked = i;
        }
        char label[32];
        snprintf(label, sizeof label, "%d selected", br.nselected);
        ui_text_box(label, (Rectangle){x, r.y, right - x - DP(8), r.height}, 17, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
        if (clicked == 0) ui_menu_open(app.menu_selection, (Rectangle){r.x + r.width - DP(4) - b, cy, b, b});
        else if (clicked == 1) ops_delete(0);
        else if (clicked == 2) ops_copy(1);
        else if (clicked == 3) ops_copy(0);
        else if (clicked == 4) browser_select_all();
        ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
        return;
    }
    if (editing) {
        if (ui_icon_button(ui_id("path-close", 0), btn, "x", theme.text)) {
            path_edit_stop();
            return;
        }
        x += b + DP(4);
        Rectangle field = {x, r.y + DP(8), right - x - DP(4), r.height - DP(16)};
        unsigned id = ui_id("path", 0);
        ui.focus = id;
        if (ui_key(KEY_TAB, 0)) complete();
        int res = ui_text_field(id, &path_field, field, "Path, e.g. ~/Downloads");
        if (res & TEXT_CANCEL) path_edit_stop();
        else if (res & TEXT_SUBMIT) path_go();
        ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
        return;
    }
    if (br.filtering) {
        if (ui_icon_button(ui_id("filter-close", 0), btn, "arrow-left", theme.text)) {
            filter_stop();
            return;
        }
        x += b + DP(4);
        Rectangle field = {x, r.y + DP(8), right - x - DP(4), r.height - DP(16)};
        int res = ui_text_field(ui_id("filter", 0), &br.filter, field, "Filter this folder");
        if (res & TEXT_CHANGED) browser_rebuild_view();
        if (res & TEXT_CANCEL) filter_stop();
        if (res & TEXT_SUBMIT) {
            ui.focus = 0;
            ui.nchars = 0; /* the field took this frame's typing; the list must not read it as commands */
            br.keyboard = 1;
        }
        ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
        return;
    }
    if (!app_wide()) {
        if (ui_icon_button(ui_id("drawer", 0), btn, "menu", theme.text)) app.drawer_open = 1, places_refresh(1);
        x += b;
    }
    Color back_c = br.back.n ? theme.text : theme.text_faint;
    if (ui_icon_button(ui_id("back", 0), (Rectangle){x, cy, b, b}, "arrow-left", back_c)) browser_back();
    x += b;
    if (!ui.touch) {
        Color fwd_c = br.forward.n ? theme.text : theme.text_faint;
        if (ui_icon_button(ui_id("forward", 0), (Rectangle){x, cy, b, b}, "arrow-right", fwd_c)) browser_forward();
        x += b;
        if (ui_icon_button(ui_id("up", 0), (Rectangle){x, cy, b, b}, "arrow-up", theme.text)) browser_up();
        x += b;
    }
    right -= b;
    if (ui_icon_button(ui_id("more", 0), (Rectangle){right, cy, b, b}, "ellipsis-vertical", theme.text))
        ui_menu_open(app.menu_more, (Rectangle){right, cy, b, b});
    if (!ui.touch) {
        right -= b;
        if (ui_icon_button(ui_id("help", 0), (Rectangle){right, cy, b, b}, "circle-help", theme.text)) app.help_open = 1;
    }
    right -= b;
    if (ui_icon_button(ui_id("search", 0), (Rectangle){right, cy, b, b}, "search", theme.text)) filter_start(0);
    draw_breadcrumb((Rectangle){x + DP(4), r.y, right - x - DP(8), r.height});
    ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
}

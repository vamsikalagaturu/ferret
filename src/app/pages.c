/* A phone's pages, chosen in its bottom bar (main.c), drawn in place of the files: Recent, the folders visited and
 * files opened last; Storage, the places, which is the drawer's state (app.drawer_open) shown as a page, so whatever
 * closes the drawer (a place picked, Back) leaves the page too. */
#include "net/net.h"
#include "views.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static fr_page shown;

void page_open(fr_page p)
{
    shown = p;
    if (p != PAGE_NONE) app.drawer_open = 0;
    ui.dirty = 1;
}

fr_page page_shown(void) { return shown; }

/* "Internal storage › Download": the folder holding uri, from the outermost place */
static char *where(const char *uri)
{
    char *parent = fr_uri_parent(uri);
    if (!parent) return fr_strdup("");
    int p = -1;
    for (int i = 0; i < app.places.n; i++)
        if (fr_uri_within(app.places.v[i].uri, parent) && (p < 0 || strlen(app.places.v[i].uri) < strlen(app.places.v[p].uri))) p = i;
    char *out;
    const char *rest;
    if (p >= 0) {
        out = fr_strdup(app.places.v[p].label);
        rest = parent + strlen(app.places.v[p].uri);
    } else if (fr_uri_local(parent)) {
        out = fr_strdup("/");
        rest = fr_uri_local(parent);
    } else {
        char *server = fr_net_label(parent);
        out = server ? server : fr_strdup(parent);
        free(parent);
        return out;
    }
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/');
        if (!end) end = rest + strlen(rest);
        char *next = fr_format("%s%s%.*s", out, strcmp(out, "/") ? " › " : "", (int)(end - rest), rest);
        free(out);
        out = next;
        rest = end;
    }
    free(parent);
    return out;
}

static void open_now(const char *entry, int n)
{
    char *uri = recent_uri(entry), *name = fr_uri_name(uri);
    if (recent_is_folder(entry)) {
        page_open(PAGE_NONE);
        browser_open(uri, 1);
    } else {
        fr_entry e = {.name = name};
        ops_open_entry(uri, &e);
    }
    free(uri), free(name);
}

static void open_favorite_now(const char *uri, int n)
{
    page_open(PAGE_NONE);
    browser_open(uri, 1);
}

/* how a folder or file of these pages is shown: its name (a place's own label), icon and where it is */
static void describe(const char *uri, int folder, char **name, char **at, const char **icon, Color *ic)
{
    *name = fr_uri_name(uri);
    if (!**name) free(*name), *name = fr_net_label(uri);
    if (!*name) *name = fr_strdup(uri);
    *at = where(uri);
    fr_kind k = kind_of(*name, folder);
    *icon = folder ? (fr_uri_local(uri) ? "folder" : fr_archive_is_uri(uri) ? "folder-archive" : "server") : kind_icon(k);
    *ic = folder ? theme.accent : kind_color(k);
    for (int p = 0; folder && p < app.places.n; p++)
        if (!strcmp(app.places.v[p].uri, uri)) {
            free(*name);
            *name = fr_strdup(app.places.v[p].label);
            *icon = app.places.v[p].icon;
            char *parent = fr_uri_parent(uri);
            if (!parent || place_of(parent) < 0) free(*at), *at = fr_strdup("Storage");
            free(parent);
            return;
        }
}

/* One row as views.c draws a file on a touchscreen: the same padding, icon box, type sizes, lines and separator.
   1 when tapped; *held when the finger stayed on it. */
static int row_draw(unsigned id, Rectangle row, Rectangle clip, const char *icon, Color ic, const char *name, const char *at, int *held)
{
    int state, tapped = ui_clickable(id, GetCollisionRec(row, clip), &state);
    *held = ui_hover(GetCollisionRec(row, clip)) && ui.rpressed && ui_live();
    if (state) ui_rect(row, state == 2 ? theme.pressed : theme.hover);
    float pad = DP(16), icon_w = DP(40), tx = row.x + pad + icon_w + DP(12), tw = row.x + row.width - pad - tx;
    ui_icon(icon, (Rectangle){row.x + pad, row.y, icon_w, row.height}, 24, ic);
    ui_text_box(name, (Rectangle){tx, row.y + DP(11), tw, DP(20)}, 16, FONT_REGULAR, theme.text, ALIGN_LEFT, 1);
    ui_text_box(at, (Rectangle){tx, row.y + DP(35), tw, DP(18)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 1);
    ui_hline(tx, row.y + row.height - DP(1), row.x + row.width - tx, ui_alpha(theme.border, 0.6f));
    return tapped;
}

static void list_draw(Rectangle r, fr_page page)
{
    static fr_scroll scroll[2];
    int fav = page == PAGE_FAVORITES, n = fav ? settings.favorites.n : settings.recent.n;
    if (!n) {
        if (fav) view_draw_empty(r, "star", "No favorites yet", "Add a folder from its menu to keep it here", 0);
        else view_draw_empty(r, "history", "Nothing here yet", "Folders you visit and files you open show here", 0);
        return;
    }
    float rh = app_row_height();
    fr_scroll *sc = &scroll[fav];
    ui_scroll(ui_id(fav ? "favorites-scroll" : "recent-scroll", 0), sc, r, n * rh);
    ui_clip_begin(r);
    for (int i = 0; i < n; i++) {
        Rectangle row = {r.x, roundf(r.y + i * rh - sc->offset), r.width, rh};
        if (row.y + rh < r.y || row.y > r.y + r.height) continue;
        const char *entry = fav ? settings.favorites.v[i] : settings.recent.v[i];
        int folder = fav || recent_is_folder(entry), held;
        char *uri = fav ? fr_strdup(entry) : recent_uri(entry), *name, *at;
        const char *icon;
        Color ic;
        describe(uri, folder, &name, &at, &icon, &ic);
        if (row_draw(ui_id(fav ? "favorite-row" : "recent-row", i), row, r, icon, ic, name, at, &held))
            app_defer(fav ? open_favorite_now : open_now, entry, 0);
        if (held && fav) place_menu_open(entry, PLACE_FAVORITE);
        free(uri), free(name), free(at);
    }
    ui_clip_end();
}

void pages_draw(Rectangle r)
{
    float hh = header_height();
    Rectangle head = {r.x, r.y, r.width, hh}, body = {r.x, r.y + hh, r.width, r.height - hh};
    ui_rect(r, theme.bg);
    if (app.drawer_open) places_draw(body);
    else list_draw(body, shown);
    ui_rect(head, theme.bg);
    const char *title = app.drawer_open ? "Storage" : shown == PAGE_FAVORITES ? "Favorites" : "Recent";
    /* as header.c's bar draws a folder's name without a back arrow */
    ui_text_box(title, (Rectangle){r.x + DP(24), r.y, r.width - DP(48), hh}, 17, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    ui_hline(r.x, r.y + hh - DP(1), r.width, theme.border);
}

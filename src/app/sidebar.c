#include "app.h"
#include "net/net.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void places_refresh_now(const char *s, int now)
{
    if (!now && ui.now - app.places_at < 3) return;
    app.places_at = ui.now;
    fr_places_free(&app.places);
    plat_places(&app.places);
}

void places_refresh(int now) { app_defer(places_refresh_now, NULL, now); }

int place_of(const char *uri)
{
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < app.places.n; i++) {
        size_t n = strlen(app.places.v[i].uri);
        if (fr_uri_within(app.places.v[i].uri, uri) && n > best_len) best = i, best_len = n;
    }
    return best;
}

static const char *const disk_verbs[] = {"", "mount", "unmount", "eject", "mount", "unmount", "power off"};
static const char *const disk_busy[] = {"", "mounting...", "unmounting...", "ejecting...", "mounting...", "unmounting...", "powering off..."};

static void open_path(const char *path)
{
    char *uri = fr_uri_from_path(path);
    browser_open(uri, 1);
    free(uri);
    app.drawer_open = 0;
}

static int device_of(const fr_disks *ds, const char *uri)
{
    for (int i = 0; uri && i < ds->n; i++)
        if (ds->v[i].uri && fr_uri_within(ds->v[i].uri, uri)) return i;
    return -1;
}

void disks_poll(void)
{
    unsigned serial = plat_disks_serial();
    if (serial != app.disks_serial) {
        app.disks_serial = serial;
        fr_disks old = app.disks;
        app.disks = (fr_disks){0};
        plat_disks(&app.disks);
        ui.dirty = 1;
        /* the device being shown was unplugged */
        if (device_of(&old, br.uri) >= 0 && device_of(&app.disks, br.uri) < 0) {
            char *home = plat_home_uri();
            browser_open(home, 1);
            free(home);
        }
        fr_disks_free(&old);
    }
    fr_disk_done d;
    while (plat_disk_done(&d)) {
        if (dview_disk_done(&d)) {
            fr_disk_done_free(&d);
            continue;
        }
        if (d.error) ui_toast("Could not %s %s: %s", disk_verbs[d.op], d.label, d.error);
        else if (d.op == DISK_MOUNT && d.path && !dview_is_open()) open_path(d.path);
        else if (br.uri) {
            int inside = 0;
            for (int i = 0; i < d.unmounted.n && !inside; i++) {
                char *m = fr_uri_from_path(d.unmounted.v[i]);
                inside = fr_uri_within(m, br.uri);
                free(m);
            }
            if (inside) {
                char *home = plat_home_uri();
                browser_open(home, 1);
                free(home);
            }
        }
        fr_disk_done_free(&d);
    }
}

static int disk_of(const char *uri)
{
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < app.disks.n; i++)
        for (int k = 0; k < app.disks.v[i].mounts.n; k++) {
            char *m = fr_uri_from_path(app.disks.v[i].mounts.v[k]);
            size_t n = strlen(m);
            if (fr_uri_within(m, uri) && n > best_len) best = i, best_len = n;
            free(m);
        }
    return best >= 0 ? best : device_of(&app.disks, uri);
}

/* the disks view's row: "Manage disks" heading the disks, "Storage" after the places where there is no disk manager */
static struct {
    unsigned id;
    char *uri;
    int server;
} place_menu;

void place_menu_open(const char *uri, int server)
{
    if (!place_menu.id) place_menu.id = ui_id("menu-place", 0);
    free(place_menu.uri);
    place_menu.uri = fr_strdup(uri);
    place_menu.server = server;
    ui_menu_open(place_menu.id, (Rectangle){ui.mouse.x, ui.mouse.y, 0, 0});
}

void place_menu_draw(void)
{
    if (!place_menu.id || !ui_menu_begin(place_menu.id)) return;
    const char *uri = place_menu.uri;
    int server = place_menu.server, i = server ? server_index(uri) : favorite_index(uri), n = server ? settings.servers.n : settings.favorites.n;
    if (i < 0) {
        ui_menu_end();
        return;
    }
    if (ui_menu_item("Open", "folder-open", -1)) {
        dview_close();
        browser_open(uri, 1);
        app.drawer_open = 0;
    }
    if (!ui.touch && ui_menu_item("Open in a new tab", "plus", -1)) tab_new(uri, 1);
    if (app_split() && ui_menu_item("Open in the other pane", "columns-2", -1)) {
        dview_close();
        pane_switch();
        browser_open(uri, 1);
    }
    ui_menu_separator();
    if (i > 0 && ui_menu_item("Move up", "arrow-up", -1)) server ? server_move(uri, -1) : favorite_move(uri, -1);
    if (i < n - 1 && ui_menu_item("Move down", "arrow-down", -1)) server ? server_move(uri, 1) : favorite_move(uri, 1);
    if (n > 1) ui_menu_separator();
    if (ui_menu_item(server ? "Remove from the sidebar" : "Remove from favorites", server ? "trash" : "star-off", -1)) {
        char *name = server ? fr_net_label(uri) : fr_uri_name(uri);
        ui_toast(server ? "Removed %s from the sidebar" : "Removed %s from favorites", *name ? name : uri);
        free(name);
        if (server) server_set(uri, 0);
        else favorite_toggle(uri);
    }
    ui_menu_end();
}

static float manage_row(Rectangle r, float y, float ih)
{
    Rectangle row = {r.x + DP(8), y, r.width - DP(16), ih};
    int state, on = dview_is_open();
    if (ui_clickable(ui_id("manage-disks", 0), row, &state)) {
        if (on) dview_close();
        else dview_open();
    }
    if (on) ui_round(row, 8, theme.selection);
    else if (state) ui_round(row, 8, state == 2 ? theme.pressed : theme.hover);
    const char *label = plat_dm_supported() ? "Manage disks" : "Storage";
    ui_icon("hard-drive",(Rectangle){row.x + DP(8), y, DP(32), ih}, 20, on ? theme.accent : theme.text_dim);
    ui_text_box(label, (Rectangle){row.x + DP(52), y, row.width - DP(60), ih}, 15, on ? FONT_SEMIBOLD : FONT_REGULAR, on ? theme.text : theme.text_dim,
                ALIGN_LEFT, 0);
    return y + ih;
}

static float disks_draw(Rectangle r, float y, float ih, int current)
{
    if (!plat_dm_supported()) {
        ui_hline(r.x + DP(16), y + DP(6), r.width - DP(32), theme.border);
        return manage_row(r, y + DP(12), ih);
    }
    ui_hline(r.x + DP(16), y + DP(6), r.width - DP(32), theme.border);
    y += DP(12);
    ui_text_box("Disks", (Rectangle){r.x + DP(20), y, r.width - DP(40), DP(24)}, 13, FONT_SEMIBOLD, theme.text_dim, ALIGN_LEFT, 0);
    y += DP(24);
    y = manage_row(r, y, ih);
    float l1 = ui_line_height(15), l2 = ui_line_height(13), dh = fmaxf(ih, l1 + l2 + DP(14));
    for (int i = 0; i < app.disks.n; i++) {
        const fr_disk *d = &app.disks.v[i];
        int mounted = d->mounts.n > 0, sel = i == current, eject = mounted && !d->busy;
        Rectangle row = {r.x + DP(8), y, r.width - DP(16), dh};
        float bs = DP(24);
        Rectangle btn = {row.x + row.width - bs - DP(4), y + (dh - bs) / 2, bs, bs};
        Rectangle hit = eject ? (Rectangle){row.x, y, btn.x - row.x, dh} : row;
        int state;
        if (ui_clickable(ui_id("disk", i), hit, &state) && !d->busy) {
            dview_close();
            if (d->uri) {
                browser_open(d->uri, 1);
                app.drawer_open = 0;
            } else if (d->locked) ui_toast("Unlocking is not supported yet");
            else if (mounted) open_path(d->mounts.v[0]);
            else plat_disk_request(DISK_MOUNT, d->id);
        }
        if (sel) ui_round(row, 8, theme.selection);
        else if (state) ui_round(row, 8, state == 2 ? theme.pressed : theme.hover);
        ui_icon(d->uri ? "smartphone" : d->locked ? "lock" : d->removable ? "usb" : "hard-drive", (Rectangle){row.x + DP(8), y, DP(32), dh},
                20, sel ? theme.accent : theme.text_dim);
        char size[32] = "", sub[192];
        if (d->size) fr_format_size(d->size, size, sizeof size);
        if (d->busy) snprintf(sub, sizeof sub, "%s", disk_busy[d->busy]);
        else snprintf(sub, sizeof sub, "%s%s%s", size, *size && d->drive ? "  ·  " : "", d->drive ? d->drive : "");
        float lx = row.x + DP(52), lw = btn.x - DP(4) - lx, ty = y + (dh - l1 - l2) / 2;
        ui_text_box(d->label, (Rectangle){lx, ty, lw, l1}, 15, sel ? FONT_SEMIBOLD : FONT_REGULAR, sel ? theme.text : theme.text_dim,
                    ALIGN_LEFT, 0);
        ui_text_box(sub, (Rectangle){lx, ty + l1, lw, l2}, 13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
        if (eject) {
            int bstate;
            if (ui_clickable(ui_id("disk-eject", i), btn, &bstate)) plat_disk_request(d->removable ? DISK_EJECT : DISK_UNMOUNT, d->id);
            if (bstate) ui_round(btn, 12, bstate == 2 ? theme.pressed : theme.hover);
            ui_icon("eject", btn, 16, theme.text_dim);
        }
        y += dh;
    }
    return y;
}

/* the places scroll when they do not fit (a phone held sideways, many favorites or disks); the desktop's mode line
 * and shortcuts row stay pinned under them */
void places_draw(Rectangle r)
{
    static fr_scroll scroll;
    static float content_h;
    ui_rect(r, theme.surface);
    float ih = ui.touch ? fmaxf(DP(52), ui_line_height(15) + DP(20)) : app_row_height(), status_h = DP(36);
    float foot = ui.touch ? 0 : ih + DP(8) + DP(8) + status_h + DP(4);
    Rectangle view = {r.x, r.y, r.width, r.height - foot};
    ui_scroll(ui_id("places-scroll", 0), &scroll, view, content_h);
    ui_clip_begin(view);
    float y = r.y + DP(12) - scroll.offset;
    ui_text_box("ferret", (Rectangle){r.x + DP(20), y, r.width - DP(40), DP(28)}, 20, FONT_SEMIBOLD, theme.accent, ALIGN_LEFT, 0);
    ui_text_box(FR_VERSION, (Rectangle){r.x + DP(20), y + DP(28), r.width - DP(40), DP(16)}, 12, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 1);
    y += DP(56);
    int fav_current = br.uri && !dview_is_open() ? favorite_index(br.uri) : -1;
    if (settings.favorites.n) {
        ui_text_box("Favorites", (Rectangle){r.x + DP(20), y, r.width - DP(40), DP(24)}, 13, FONT_SEMIBOLD, theme.text_dim, ALIGN_LEFT, 0);
        y += DP(24);
    }
    for (int i = 0; i < settings.favorites.n; i++) {
        const char *uri = settings.favorites.v[i];
        Rectangle row = {r.x + DP(8), y, r.width - DP(16), ih};
        int state;
        char *name = fr_uri_name(uri);
        if (!*name) free(name), name = fr_strdup(fr_uri_local(uri) ? "/" : uri);
        if (ui_hover(row) && ui.rpressed && ui_live()) place_menu_open(uri, 0);
        if (ui_clickable(ui_id("favorite", i), row, &state)) {
            dview_close();
            browser_open(uri, 1);
            app.drawer_open = 0;
        }
        if (i == fav_current) ui_round(row, 8, theme.selection);
        else if (state) ui_round(row, 8, state == 2 ? theme.pressed : theme.hover);
        ui_icon("star", (Rectangle){row.x + DP(8), y, DP(32), ih}, 20, i == fav_current ? theme.accent : theme.text_dim);
        ui_text_box(name, (Rectangle){row.x + DP(52), y, row.width - DP(60), ih}, 15, i == fav_current ? FONT_SEMIBOLD : FONT_REGULAR,
                    i == fav_current ? theme.text : theme.text_dim, ALIGN_LEFT, 0);
        free(name);
        y += ih;
    }
    if (settings.favorites.n) {
        ui_hline(r.x + DP(16), y + DP(6), r.width - DP(32), theme.border);
        y += DP(12);
    }
    fr_places *p = &app.places;
    int disk_current = br.uri && !dview_is_open() && fav_current < 0 ? disk_of(br.uri) : -1;
    int current = br.uri && !dview_is_open() && fav_current < 0 && disk_current < 0 ? place_of(br.uri) : -1;
    for (int i = 0; i < p->n; i++) {
        if (i && p->v[i].removable && !p->v[i - 1].removable) {
            ui_hline(r.x + DP(16), y + DP(6), r.width - DP(32), theme.border);
            y += DP(12);
        }
        Rectangle row = {r.x + DP(8), y, r.width - DP(16), ih};
        int state;
        if (ui_clickable(ui_id("place", i), row, &state)) {
            dview_close();
            browser_open(p->v[i].uri, 1);
            app.drawer_open = 0;
        }
        if (i == current) ui_round(row, 8, theme.selection);
        else if (state) ui_round(row, 8, state == 2 ? theme.pressed : theme.hover);
        Color c = i == current ? theme.accent : theme.text_dim;
        ui_icon(p->v[i].icon, (Rectangle){row.x + DP(8), y, DP(32), ih}, 20, c);
        ui_text_box(p->v[i].label, (Rectangle){row.x + DP(52), y, row.width - DP(60), ih}, 15,
                    i == current ? FONT_SEMIBOLD : FONT_REGULAR, i == current ? theme.text : theme.text_dim, ALIGN_LEFT, 0);
        y += ih;
    }
    y = disks_draw(r, y, ih, disk_current);
    y = network_draw(r, y, ih);
    ui_clip_end();
    float content = y + scroll.offset - r.y + DP(8);
    if (fabsf(content - content_h) > 0.5f) content_h = content, ui.dirty = 1;
    if (ui.touch) return;
    Rectangle help = {r.x + DP(8), r.y + r.height - ih - DP(8), r.width - DP(16), ih};
    Rectangle status = {r.x + DP(16), help.y - DP(8) - status_h, r.width - DP(32), status_h};
    /* rows scrolled under the pinned part take no clicks */
    ui_clickable(ui_id("places-foot", 0), (Rectangle){r.x, view.y + view.height, r.width, foot}, NULL);
    ui_rect((Rectangle){r.x, view.y + view.height, r.width, foot}, theme.surface);
    ui_hline(r.x + DP(16), status.y - DP(4), r.width - DP(32), theme.border);
    static const char *const modes[] = {"NORMAL", "VISUAL", "INSERT"};
    int mode = keys_mode();
    Color pill_c = mode == MODE_INSERT ? theme.success : mode == MODE_VISUAL ? theme.warning : theme.accent;
    float pw = ui_text_width(modes[mode], 12, FONT_SEMIBOLD) + DP(16), ph = DP(22);
    Rectangle pill = {status.x, status.y + (status_h - ph) / 2, pw, ph};
    ui_round(pill, 6, pill_c);
    ui_text_box(modes[mode], pill, 12, FONT_SEMIBOLD, theme.on_accent, ALIGN_CENTER, 0);
    char count[96];
    int hidden = 0;
    for (int i = 0; !br.show_hidden && i < br.items.n; i++) hidden += br.items.v[i].e.hidden;
    int n = br.filtering && br.filter.len ? snprintf(count, sizeof count, "%d %s", br.view.n, br.view.n == 1 ? "match" : "matches")
                                          : snprintf(count, sizeof count, "%d %s", br.view.n, br.view.n == 1 ? "item" : "items");
    if (hidden) n += snprintf(count + n, sizeof count - (size_t)n, ", %d hidden", hidden);
    if (br.nselected) snprintf(count + n, sizeof count - (size_t)n, ", %d selected", br.nselected);
    ui_text_box(count, (Rectangle){pill.x + pw + DP(10), status.y, status.width - pw - DP(10), status_h}, 14, FONT_REGULAR, theme.text_dim,
                ALIGN_LEFT, 0);
    int state;
    if (ui_clickable(ui_id("help", 0), help, &state)) app.help_open = 1;
    if (state) ui_round(help, 8, state == 2 ? theme.pressed : theme.hover);
    ui_icon("keyboard", (Rectangle){help.x + DP(8), help.y, DP(32), ih}, 20, theme.text_dim);
    float cap = DP(22);
    Rectangle key = {help.x + help.width - DP(10) - cap, help.y + (ih - cap) / 2, cap, cap};
    ui_text_box("Keyboard shortcuts", (Rectangle){help.x + DP(52), help.y, key.x - help.x - DP(60), ih}, 15, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    ui_round_line(key, 5, theme.border);
    ui_text_box("?", key, 13, FONT_SEMIBOLD, theme.text_dim, ALIGN_CENTER, 0);
}

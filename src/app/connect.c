/* Network locations in the window, for any scheme with a VFS backend: the sidebar's Network section (the saved
 * servers and "Connect to server"), the dialog that takes an address, and what a server can ask of the user before
 * it lists anything: a user name and password (kept in the system's keystore when asked to), or trust in its
 * identity the first time (an SSH host key). */
#include "app.h"
#include "net/net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static enum { NONE, CONNECT, LOGIN, TRUST } dlg;
static fr_text address, user, password;
static int save_server = 1, remember;
static char *dlg_key, *dlg_question; /* LOGIN, TRUST: the server; TRUST: what it asks */
static void (*dlg_retry)(void);
static char title[160], message[512];

char *server_uri(const char *typed)
{
    char *uri = fr_net_normalize(typed);
    if (!uri || fr_uri_local(uri) || !fr_vfs_backend(uri)) {
        free(uri);
        return NULL;
    }
    return uri;
}

void net_prepare(const char *uri)
{
    fr_net_uri u;
    if (fr_net_parse(uri, &u)) return;
    /* only servers that sign in have passwords (not mtp://, archive://) */
    if (fr_net_default_port(u.scheme)) {
        char *key = fr_net_key(&u);
        fr_creds_preload(key);
        free(key);
    }
    fr_net_uri_free(&u);
}

static void close_dialog(void)
{
    dlg = NONE;
    free(dlg_key), free(dlg_question);
    dlg_key = dlg_question = NULL;
    memset(password.buf, 0, sizeof password.buf);
    text_set(&password, "");
    ui.dirty = 1;
}

int net_ask(const char *uri, int rc, void (*retry)(void))
{
    if (rc != -FR_EAUTH && rc != -FR_EUNTRUSTED) return 0;
    fr_net_uri u;
    if (fr_net_parse(uri, &u)) return 0;
    char *key = fr_net_key(&u);
    if (dlg != NONE && dlg != CONNECT) {
        int same = dlg_key && !strcmp(dlg_key, key);
        free(key);
        fr_net_uri_free(&u);
        return same;
    }
    char *question = rc == -FR_EUNTRUSTED ? fr_trust_question(key) : NULL;
    if (rc == -FR_EUNTRUSTED && !question) {
        free(key);
        fr_net_uri_free(&u);
        return 0;
    }
    close_dialog();
    dlg_key = key, dlg_question = question, dlg_retry = retry;
    if (question) {
        dlg = TRUST;
        snprintf(title, sizeof title, "Trust %s?", u.host);
    } else {
        dlg = LOGIN;
        snprintf(title, sizeof title, "Sign in to %s", u.host);
        snprintf(message, sizeof message, "%s", fr_creds_refused(key) ? "The user name or password was not accepted."
                                                 : "The server asks for a user name and password.");
        const char *who = u.user;
#ifndef __ANDROID__
        if (!who && !strcmp(u.scheme, "sftp")) who = getenv("USER");
#endif
        text_set(&user, who ? who : "");
        text_set(&password, "");
        password.secret = 1;
        remember = 0;
    }
    fr_net_uri_free(&u);
    return 1;
}

void connect_start(void)
{
    close_dialog();
    text_set(&address, "");
    save_server = 1;
    dlg = CONNECT;
}

int connect_dialog_open(void) { return dlg != NONE; }

static void connect_commit(void)
{
    char *uri = server_uri(address.buf);
    if (!uri) {
        ui_toast("ferret cannot open that: an address starts with smb://, sftp://, ftp(s)://, dav(s):// or nfs://");
        return;
    }
    if (save_server) server_set(uri, 1);
    browser_open(uri, 1);
    app.drawer_open = 0;
    free(uri);
    close_dialog();
}

void connect_draw_dialogs(void)
{
    int b;
    switch (dlg) {
    case CONNECT: {
        fr_dialog d = {.title = "Connect to server", .message = "An address such as smb://nas/share, sftp://user@host/path, ftps://host, davs://host/dav or nfs://nas/export.",
                       .field = &address, .hint = "smb://nas/share", .check = "Show it in the sidebar",
                       .checked = &save_server, .buttons = {"Cancel", "Connect"}, .nbuttons = 2, .cancel = 0};
        b = ui_dialog(&d);
        if (b == 1) connect_commit();
        else if (b == 0) close_dialog();
        break;
    }
    case LOGIN: {
        fr_dialog d = {.title = title, .message = message, .field = &user, .field2 = &password, .hint = "User name",
                       .hint2 = "Password", .check = fr_creds_store_available() ? "Remember the password" : NULL,
                       .checked = &remember, .buttons = {"Cancel", "Sign in"}, .nbuttons = 2, .cancel = 0};
        b = ui_dialog(&d);
        if (b == 1 && !user.len) ui_toast("Type a user name");
        else if (b == 1) {
            fr_creds_put(dlg_key, user.buf, password.buf, remember);
            void (*retry)(void) = dlg_retry;
            close_dialog();
            if (retry) retry();
        } else if (b == 0) close_dialog();
        break;
    }
    case TRUST: {
        fr_dialog d = {.title = title, .message = dlg_question, .buttons = {"Cancel", "Trust"}, .nbuttons = 2, .cancel = 0};
        b = ui_dialog(&d);
        if (b >= 0) {
            fr_trust_answer(dlg_key, b == 1);
            void (*retry)(void) = b == 1 ? dlg_retry : NULL;
            close_dialog();
            if (retry) retry();
        }
        break;
    }
    default: break;
    }
}

/* ---- the sidebar's Network section ---- */

float network_draw(Rectangle r, float y, float ih)
{
    ui_hline(r.x + DP(16), y + DP(6), r.width - DP(32), theme.border);
    y += DP(12);
    ui_text_box("Network", (Rectangle){r.x + DP(20), y, r.width - DP(40), DP(24)}, 13, FONT_SEMIBOLD, theme.text_dim, ALIGN_LEFT, 0);
    y += DP(24);
    int current = -1;
    size_t best = 0;
    for (int i = 0; br.uri && i < settings.servers.n; i++) {
        size_t n = strlen(settings.servers.v[i]);
        if (fr_uri_within(settings.servers.v[i], br.uri) && n > best) current = i, best = n;
    }
    for (int i = 0; i < settings.servers.n; i++) {
        const char *uri = settings.servers.v[i];
        Rectangle row = {r.x + DP(8), y, r.width - DP(16), ih};
        char *label = fr_net_label(uri);
        if (ui_hover(row) && ui.rpressed && ui_live()) place_menu_open(uri, PLACE_SERVER);
        int state, sel = i == current;
        if (ui_clickable(ui_id("server", i), row, &state)) {
            browser_open(uri, 1);
            app.drawer_open = 0;
        }
        if (sel) ui_round(row, 8, theme.selection);
        else if (state) ui_round(row, 8, state == 2 ? theme.pressed : theme.hover);
        ui_icon("server", (Rectangle){row.x + DP(8), y, DP(32), ih}, 20, sel ? theme.accent : theme.text_dim);
        char scheme[16];
        size_t sn = (size_t)(strstr(uri, "://") - uri);
        for (size_t k = 0; k < sn && k < sizeof scheme - 1; k++) scheme[k] = (char)(uri[k] >= 'a' && uri[k] <= 'z' ? uri[k] - 32 : uri[k]);
        scheme[sn < sizeof scheme - 1 ? sn : sizeof scheme - 1] = 0;
        float right = row.x + row.width - DP(8), nw = ui_text_width(scheme, 12, FONT_REGULAR);
        ui_text_box(scheme, (Rectangle){right - nw, y, nw, ih}, 12, FONT_REGULAR, theme.text_faint, ALIGN_RIGHT, 0);
        float lx = row.x + DP(52);
        ui_text_box(label, (Rectangle){lx, y, right - nw - DP(8) - lx, ih}, 15, sel ? FONT_SEMIBOLD : FONT_REGULAR,
                    sel ? theme.text : theme.text_dim, ALIGN_LEFT, 0);
        free(label);
        y += ih;
    }
    Rectangle add = {r.x + DP(8), y, r.width - DP(16), ih};
    int state;
    if (ui_clickable(ui_id("server-add", 0), add, &state)) connect_start();
    if (state) ui_round(add, 8, state == 2 ? theme.pressed : theme.hover);
    ui_icon("plus", (Rectangle){add.x + DP(8), y, DP(32), ih}, 20, theme.text_dim);
    ui_text_box("Connect to server", (Rectangle){add.x + DP(52), y, add.width - DP(60), ih}, 15, FONT_REGULAR, theme.text_dim,
                ALIGN_LEFT, 0);
    return y + ih;
}

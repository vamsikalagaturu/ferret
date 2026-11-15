#define _GNU_SOURCE
#include "app.h"
#include "core/toml.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

fr_settings settings;

#define COUNT(a) ((int)(sizeof(a) / sizeof *(a)))
/* by name, so a value this build does not know (or no longer has) is skipped */
static const char *const sort_names[] = {[SORT_NAME] = "name", [SORT_SIZE] = "size", [SORT_DATE] = "modified", [SORT_KIND] = "kind", [SORT_EXT] = "extension"};
static const char *const layout_names[] = {[LAYOUT_DETAILS] = "details", [LAYOUT_COLUMNS] = "columns", [LAYOUT_GRID] = "grid"};

typedef struct {
    fr_sort sort;
    int desc, hidden;
    fr_layout layout;
    float ui_scale, font_scale;
    fr_strs favorites, servers;
    char *folder;
    fr_strs tabs; /* owned, unlike the others in live()'s */
    fr_bookmarks bookmarks;
} state;

static char *path;
static state saved, seen; /* as last written or read; as last seen changing */
static int saved_valid;
static double seen_at, checked_at;
static int64_t file_mtime;

/* the folders worth restoring: local ones (and the trash) as usable_start() takes them, in the tabs' order; for a
 * split tab, its pane with the keys */
static fr_strs tab_folders(void)
{
    fr_strs r = {0};
    for (int i = 0; tabs.n > 1 && i < tabs.n; i++) {
        const fr_browser *b = tabs.v[i].pane[tabs.v[i].side];
        if (b->uri && (fr_uri_local(b->uri) || !strncmp(b->uri, "trash://", 8))) FR_PUSH(r, fr_strdup(b->uri));
    }
    return r;
}

static state live(void)
{
    return (state){br.sort, br.desc, br.show_hidden, br.layout, ui.user_scale, ui.text_scale, settings.favorites, settings.servers, br.uri,
                   tab_folders(), settings.bookmarks};
}

static int strs_differ(const fr_strs *a, const fr_strs *b)
{
    if (a->n != b->n) return 1;
    for (int i = 0; i < a->n; i++)
        if (strcmp(a->v[i], b->v[i])) return 1;
    return 0;
}

static int differs(const state *a, const state *b)
{
    if (!a->folder != !b->folder || (a->folder && strcmp(a->folder, b->folder))) return 1;
    return a->sort != b->sort || a->desc != b->desc || a->hidden != b->hidden || a->layout != b->layout ||
           a->ui_scale != b->ui_scale || a->font_scale != b->font_scale || strs_differ(&a->favorites, &b->favorites) ||
           strs_differ(&a->servers, &b->servers) || strs_differ(&a->tabs, &b->tabs) || !fr_bookmarks_equal(&a->bookmarks, &b->bookmarks);
}

static void snapshot(state *s)
{
    fr_strs_free(&s->favorites);
    fr_strs_free(&s->servers);
    fr_strs_free(&s->tabs);
    fr_bookmarks_free(&s->bookmarks);
    free(s->folder);
    *s = live();
    s->bookmarks = fr_bookmarks_copy(&settings.bookmarks);
    s->favorites = fr_strs_copy(&settings.favorites);
    s->servers = fr_strs_copy(&settings.servers);
    s->folder = s->folder ? fr_strdup(s->folder) : NULL;
}

/* $XDG_CONFIG_HOME, else ~/.config (XDG Base Directory Specification 0.8: a relative value is ignored) */
static char *settings_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/') return fr_format("%s/ferret/settings.toml", xdg);
    const char *home = getenv("HOME");
    return home && *home ? fr_format("%s/.config/ferret/settings.toml", home) : NULL;
}

static int64_t mtime_of(const char *p)
{
    struct stat st;
    if (stat(p, &st)) return -1;
    return (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
}

static char *read_file(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fr_toml_out o = {0};
    char buf[4096];
    size_t n;
    FR_GROW(o, 1);
    o.v[0] = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        FR_GROW(o, o.n + (int)n + 1);
        memcpy(o.v + o.n, buf, n);
        o.n += (int)n;
        o.v[o.n] = 0;
    }
    fclose(f);
    return o.v;
}

static int find(const char *const *names, int n, const char *s)
{
    for (int i = 0; i < n; i++)
        if (names[i] && !strcmp(names[i], s)) return i;
    return -1;
}

static char *favorite_uri(const char *s)
{
    if (*s == '/') {
        size_t n = strlen(s);
        while (n > 1 && s[n - 1] == '/') n--;
        char *p = fr_strndup(s, n), *uri = fr_uri_from_path(p);
        free(p);
        return uri;
    }
    return strstr(s, "://") ? fr_strdup(s) : NULL;
}

static char *start_written; /* start_folder as the user wrote it, written back the same */

/* a path ("~" for home), else a URI */
static char *folder_uri(const char *s)
{
    const char *home = getenv("HOME");
    if (home && s[0] == '~' && (!s[1] || s[1] == '/')) {
        char *p = fr_format("%s%s", home, s + 1), *uri = favorite_uri(p);
        free(p);
        return uri;
    }
    return favorite_uri(s);
}

static void apply(const fr_toml *t)
{
    const fr_toml_value *v;
    int i;
    if ((v = fr_toml_get(t, "", "layout", TOML_STRING)) && (i = find(layout_names, COUNT(layout_names), v->str)) >= 0)
        br.layout = (fr_layout)i;
    if ((v = fr_toml_get(t, "", "sort", TOML_STRING)) && (i = find(sort_names, COUNT(sort_names), v->str)) >= 0)
        br.sort = (fr_sort)i;
    if ((v = fr_toml_get(t, "", "reverse", TOML_BOOL))) br.desc = v->b;
    if ((v = fr_toml_get(t, "", "show_hidden", TOML_BOOL))) br.show_hidden = v->b;
    if ((v = fr_toml_get(t, "", "ui_scale", TOML_NUMBER)) && v->num >= UI_SCALE_MIN && v->num <= UI_SCALE_MAX)
        ui.user_scale = (float)v->num;
    if ((v = fr_toml_get(t, "", "font_scale", TOML_NUMBER)) && v->num >= TEXT_SCALE_MIN && v->num <= TEXT_SCALE_MAX)
        ui.text_scale = (float)v->num;
    if ((v = fr_toml_get(t, "", "favorites", TOML_STRINGS))) {
        fr_strs_free(&settings.favorites);
        for (i = 0; i < v->strs.n; i++) {
            char *uri = favorite_uri(v->strs.v[i]);
            if (uri && favorite_index(uri) < 0) FR_PUSH(settings.favorites, uri);
            else free(uri);
        }
    }
    const char *folder_keys[2] = {"last_folder", "start_folder"};
    char **folders[2] = {&settings.last_folder, &settings.start_folder};
    for (i = 0; i < 2; i++) {
        free(*folders[i]);
        v = fr_toml_get(t, "", folder_keys[i], TOML_STRING);
        *folders[i] = v ? folder_uri(v->str) : NULL;
    }
    fr_strs_free(&settings.last_tabs);
    if ((v = fr_toml_get(t, "", "last_tabs", TOML_STRINGS)))
        for (i = 0; i < v->strs.n; i++) {
            char *uri = folder_uri(v->strs.v[i]);
            if (uri) FR_PUSH(settings.last_tabs, uri);
        }
    free(start_written);
    v = fr_toml_get(t, "", "start_folder", TOML_STRING);
    start_written = v ? fr_strdup(v->str) : NULL;
    if ((v = fr_toml_get(t, "", "servers", TOML_STRINGS))) {
        fr_strs_free(&settings.servers);
        for (i = 0; i < v->strs.n; i++) {
            char *uri = server_uri(v->strs.v[i]);
            if (uri && server_index(uri) < 0) FR_PUSH(settings.servers, uri);
            else free(uri);
        }
    }
    fr_bookmarks_parse(&settings.bookmarks, t, folder_uri);
    for (i = 0; i < THEME_ROLES; i++) {
        free(settings.theme[i]);
        v = fr_toml_get(t, "theme", theme_roles[i], TOML_STRING);
        settings.theme[i] = v ? fr_strdup(v->str) : NULL;
    }
}

/* the file into the settings; 0 when there is none */
static int read_settings(void)
{
    file_mtime = mtime_of(path);
    char *text = read_file(path);
    if (!text) return 0;
    fr_toml t = fr_toml_parse(text);
    free(text);
    if (t.error_line) fprintf(stderr, "ferret: %s:%d: not understood, skipped\n", path, t.error_line);
    apply(&t);
    fr_toml_free(&t);
    snapshot(&saved);
    snapshot(&seen);
    saved_valid = 1;
    return 1;
}

static char *settings_text(void)
{
    fr_toml_out o = {0};
    fr_toml_comment(&o, "ferret's settings. ferret rewrites this file when they change in the app, and reads it");
    fr_toml_comment(&o, "again a few seconds after you edit it.");
    fr_toml_comment(&o, "");
    fr_toml_comment(&o, "How files are shown: \"details\", \"columns\" or \"grid\".");
    fr_toml_string(&o, "layout", (int)br.layout < COUNT(layout_names) ? layout_names[br.layout] : layout_names[0]);
    fr_toml_comment(&o, "Sort by \"name\", \"size\", \"modified\", \"kind\" or \"extension\".");
    fr_toml_string(&o, "sort", (int)br.sort < COUNT(sort_names) && sort_names[br.sort] ? sort_names[br.sort] : sort_names[0]);
    fr_toml_comment(&o, "Sort in reverse order.");
    fr_toml_bool(&o, "reverse", br.desc);
    fr_toml_comment(&o, "Show hidden files (names starting with a dot).");
    fr_toml_bool(&o, "show_hidden", br.show_hidden);
    fr_toml_comment(&o, "Interface size over the system's scale: 1.0 as the system says, 0.5 to 2.0 (Ctrl +, Ctrl -, Ctrl 0).");
    fr_toml_number(&o, "ui_scale", ui.user_scale > 0 ? ui.user_scale : 1);
    fr_toml_comment(&o, "Text size over the interface size: 1.0 as designed, 0.7 to 2.0 (Ctrl Alt +, Ctrl Alt -, Ctrl Alt 0).");
    fr_toml_number(&o, "font_scale", ui.text_scale > 0 ? ui.text_scale : 1);
    fr_toml_comment(&o, "Folders at the top of the sidebar, in order: paths, or URIs for other storage.");
    fr_strs shown = {0};
    for (int i = 0; i < settings.favorites.n; i++) {
        const char *p = fr_uri_local(settings.favorites.v[i]);
        FR_PUSH(shown, fr_strdup(p ? p : settings.favorites.v[i]));
    }
    fr_toml_strings(&o, "favorites", &shown);
    fr_strs_free(&shown);
    fr_toml_comment(&o, "The folder open when ferret last ran; it starts there again when the folder is still there.");
    if (br.uri) {
        const char *p = fr_uri_local(br.uri);
        fr_toml_string(&o, "last_folder", p ? p : br.uri);
    }
    fr_strs open = tab_folders();
    if (open.n > 1) {
        fr_toml_comment(&o, "The tabs open then, in order (folders on this device); last_folder is the one shown.");
        for (int i = 0; i < open.n; i++) {
            const char *p = fr_uri_local(open.v[i]);
            if (p) {
                char *path = fr_strdup(p);
                free(open.v[i]);
                open.v[i] = path;
            }
        }
        fr_toml_strings(&o, "last_tabs", &open);
    }
    fr_strs_free(&open);
    fr_toml_comment(&o, "Start in this folder every time instead (a path, \"~\" for home, or a URI), for example:");
    if (start_written) fr_toml_string(&o, "start_folder", start_written);
    else fr_toml_comment(&o, "start_folder = \"~/Downloads\"");
    fr_toml_comment(&o, "Servers in the sidebar's Network section: smb://[user@]host/share, sftp://[user@]host[:port]/path,");
    fr_toml_comment(&o, "ftp:// and ftps:// (TLS), dav:// and davs:// (WebDAV over http and https), nfs://host/export.");
    fr_toml_comment(&o, "Passwords are not kept here: the system's keystore has them when you ask ferret to remember one.");
    fr_toml_strings(&o, "servers", &settings.servers);
    fr_toml_comment(&o, "");
    fr_toml_comment(&o, "Bookmarks: m and a letter or digit marks a folder, ' and the key goes there; key = \"path or URI\".");
    fr_bookmarks_write(&o, &settings.bookmarks, fr_uri_local);
    fr_toml_comment(&o, "");
    fr_toml_table(&o, "theme");
    fr_toml_comment(&o, "Colours as \"#rrggbb\", over the desktop's and the Nord defaults, for any of: accent,");
    fr_toml_comment(&o, "background, dark_background, lighter_background, foreground, bright_foreground, red,");
    fr_toml_comment(&o, "yellow, green, blue, magenta, cyan, orange. For example: accent = \"#88c0d0\"");
    for (int i = 0; i < THEME_ROLES; i++)
        if (settings.theme[i]) fr_toml_string(&o, theme_roles[i], settings.theme[i]);
    return o.v;
}

static int write_atomically(const char *target, const char *text)
{
    char *tmp = fr_format("%s.tmp", target);
    FILE *f = fopen(tmp, "w");
    int ok = f && fputs(text, f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    int err = errno;
    if (f && fclose(f)) ok = 0, err = errno;
    if (ok && rename(tmp, target)) ok = 0, err = errno;
    if (!ok && f) unlink(tmp);
    free(tmp);
    errno = err;
    return ok;
}

static void save(void)
{
    char *dir = fr_strndup(path, (size_t)(strrchr(path, '/') - path));
    fr_mkdirs(dir);
    free(dir);
    char *real = realpath(path, NULL), *text = settings_text();
    if (!write_atomically(real ? real : path, text))
        fprintf(stderr, "ferret: could not save %s: %s\n", path, strerror(errno));
    free(real);
    free(text);
    snapshot(&saved);
    saved_valid = 1;
    file_mtime = mtime_of(path);
}

void settings_load(void)
{
    path = settings_path();
    if (path) read_settings();
}

void settings_poll(void)
{
    if (!path) return;
    state now = live();
    if (differs(&now, &seen)) {
        snapshot(&seen);
        seen_at = ui.now;
    }
    fr_strs_free(&now.tabs);
    if (!saved_valid || differs(&seen, &saved)) {
        if (ui.now - seen_at >= 0.5) save();
        else ui_redraw_in(0.5 - (ui.now - seen_at));
        return;
    }
    if (ui.now - checked_at < 2) return;
    checked_at = ui.now;
    if (mtime_of(path) == file_mtime) return;
    if (!read_settings()) {
        saved_valid = 0;
        return;
    }
    browser_rebuild_view();
    view_set_layout(br.layout);
    theme_load(settings.theme);
}

/* only local folders and the trash: a network share that is gone would hold up the start */
static int usable_start(const char *uri)
{
    if (!uri || !(fr_uri_local(uri) || !strncmp(uri, "trash://", 8))) return 0;
    fr_entry e = {0};
    int ok = !fr_vfs_stat(uri, &e) && e.is_dir;
    fr_entry_free(&e);
    return ok;
}

char *settings_start_uri(void)
{
    if (usable_start(settings.start_folder)) return fr_strdup(settings.start_folder);
    if (usable_start(settings.last_folder)) return fr_strdup(settings.last_folder);
    return NULL;
}

fr_strs settings_start_tabs(int *current)
{
    fr_strs r = {0};
    *current = -1;
    if (settings.start_folder) return r;
    for (int i = 0; i < settings.last_tabs.n; i++) {
        if (!usable_start(settings.last_tabs.v[i])) continue;
        if (*current < 0 && settings.last_folder && !strcmp(settings.last_folder, settings.last_tabs.v[i])) *current = r.n;
        FR_PUSH(r, fr_strdup(settings.last_tabs.v[i]));
    }
    if (r.n < 2) fr_strs_free(&r), *current = -1;
    return r;
}

int favorite_index(const char *uri)
{
    for (int i = 0; i < settings.favorites.n; i++)
        if (!strcmp(settings.favorites.v[i], uri)) return i;
    return -1;
}

static void favorite_toggle_now(const char *uri, int n)
{
    int i = favorite_index(uri);
    if (i < 0) {
        FR_PUSH(settings.favorites, fr_strdup(uri));
        return;
    }
    free(settings.favorites.v[i]);
    memmove(settings.favorites.v + i, settings.favorites.v + i + 1, sizeof *settings.favorites.v * (size_t)(settings.favorites.n - i - 1));
    settings.favorites.n--;
}

void favorite_toggle(const char *uri) { app_defer(favorite_toggle_now, uri, 0); }

int server_index(const char *uri)
{
    for (int i = 0; i < settings.servers.n; i++)
        if (!strcmp(settings.servers.v[i], uri)) return i;
    return -1;
}

static void server_set_now(const char *uri, int on)
{
    int i = server_index(uri);
    if (on && i < 0) FR_PUSH(settings.servers, fr_strdup(uri));
    if (on || i < 0) return;
    free(settings.servers.v[i]);
    memmove(settings.servers.v + i, settings.servers.v + i + 1, sizeof *settings.servers.v * (size_t)(settings.servers.n - i - 1));
    settings.servers.n--;
}

void server_set(const char *uri, int on) { app_defer(server_set_now, uri, on); }

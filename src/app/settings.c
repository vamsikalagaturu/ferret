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
static const char *const sort_names[] = {[SORT_NAME] = "name", [SORT_SIZE] = "size", [SORT_DATE] = "modified", [SORT_KIND] = "kind"};
static const char *const layout_names[] = {[LAYOUT_DETAILS] = "details", [LAYOUT_COLUMNS] = "columns", [LAYOUT_GRID] = "grid"};

typedef struct {
    fr_sort sort;
    int desc, hidden;
    fr_layout layout;
    fr_strs favorites;
} state;

static char *path;
static state saved, seen; /* as last written or read; as last seen changing */
static int saved_valid;
static double seen_at, checked_at;
static int64_t file_mtime;

static state live(void) { return (state){br.sort, br.desc, br.show_hidden, app.layout, settings.favorites}; }

static int differs(const state *a, const state *b)
{
    if (a->sort != b->sort || a->desc != b->desc || a->hidden != b->hidden || a->layout != b->layout ||
        a->favorites.n != b->favorites.n)
        return 1;
    for (int i = 0; i < a->favorites.n; i++)
        if (strcmp(a->favorites.v[i], b->favorites.v[i])) return 1;
    return 0;
}

static void snapshot(state *s)
{
    fr_strs_free(&s->favorites);
    *s = live();
    s->favorites = fr_strs_copy(&settings.favorites);
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

static void apply(const fr_toml *t)
{
    const fr_toml_value *v;
    int i;
    if ((v = fr_toml_get(t, "", "layout", TOML_STRING)) && (i = find(layout_names, COUNT(layout_names), v->str)) >= 0)
        app.layout = (fr_layout)i;
    if ((v = fr_toml_get(t, "", "sort", TOML_STRING)) && (i = find(sort_names, COUNT(sort_names), v->str)) >= 0)
        br.sort = (fr_sort)i;
    if ((v = fr_toml_get(t, "", "reverse", TOML_BOOL))) br.desc = v->b;
    if ((v = fr_toml_get(t, "", "show_hidden", TOML_BOOL))) br.show_hidden = v->b;
    if ((v = fr_toml_get(t, "", "favorites", TOML_STRINGS))) {
        fr_strs_free(&settings.favorites);
        for (i = 0; i < v->strs.n; i++) {
            char *uri = favorite_uri(v->strs.v[i]);
            if (uri && favorite_index(uri) < 0) FR_PUSH(settings.favorites, uri);
            else free(uri);
        }
    }
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
    fr_toml_string(&o, "layout", (int)app.layout < COUNT(layout_names) ? layout_names[app.layout] : layout_names[0]);
    fr_toml_comment(&o, "Sort by \"name\", \"size\", \"modified\", \"kind\" or \"extension\".");
    fr_toml_string(&o, "sort", (int)br.sort < COUNT(sort_names) && sort_names[br.sort] ? sort_names[br.sort] : sort_names[0]);
    fr_toml_comment(&o, "Sort in reverse order.");
    fr_toml_bool(&o, "reverse", br.desc);
    fr_toml_comment(&o, "Show hidden files (names starting with a dot).");
    fr_toml_bool(&o, "show_hidden", br.show_hidden);
    fr_toml_comment(&o, "Folders at the top of the sidebar, in order: paths, or URIs for other storage.");
    fr_strs shown = {0};
    for (int i = 0; i < settings.favorites.n; i++) {
        const char *p = fr_uri_local(settings.favorites.v[i]);
        FR_PUSH(shown, fr_strdup(p ? p : settings.favorites.v[i]));
    }
    fr_toml_strings(&o, "favorites", &shown);
    fr_strs_free(&shown);
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
    view_set_layout(app.layout);
    theme_load(settings.theme);
}

int favorite_index(const char *uri)
{
    for (int i = 0; i < settings.favorites.n; i++)
        if (!strcmp(settings.favorites.v[i], uri)) return i;
    return -1;
}

void favorite_toggle(const char *uri)
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

/* ferret --replay SCRIPT [FOLDER]: the script's keys, text and clicks go into the input queue between frames, as
 * GLFW would deliver them, and its expectations are checked against the app's state. One command per line:
 *   key SPEC          j, G, ctrl+c, shift+down, enter, esc, f2, alt+left (US layout: ? is shift+/)
 *   type TEXT         the text as typed characters
 *   click|rclick|dclick TARGET, move TARGET
 *                     TARGET: "X Y" in window pixels (negative: from the right or bottom edge), "row NAME" (a row or
 *                     tile of the folder shown), "place LABEL" (a sidebar row: a place or a favorite)
 *   wheel DY, wait FRAMES, shot FILE.png, quit
 *   expect folder|cursor|selected|exists|missing|mode|clipboard|layout|toast VALUE
 * Between commands the window runs until it is idle: nothing to redraw, no queued input, no job running, no
 * thumbnail being made.
 * Relative paths start at FOLDER. Failures go to stderr as SCRIPT:LINE: ...; the exit status is 1 after any. */
#define _GNU_SOURCE
#include "app.h"
#include "rlgl.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define IDLE_FRAMES_MAX 120
/* past input.c's 0.4 s, so separate clicks on one spot never make a double click */
#define CLICK_GAP_S 0.6
#define TYPE_MAX 200

typedef struct {
    int line;
    char *verb, *arg;
} command;

static struct {
    int on;
    const char *script;
    struct {
        command *v;
        int n, cap;
    } cmds;
    int at;
    int wait, settling;
    int failures;
    char *base;
    Vector2 pointer;
    double pressed_at;
    const command *shot; /* taken from the next frame drawn */
} rp;

int replay_running(void) { return rp.on; }
Vector2 replay_pointer(void) { return rp.pointer; }

__attribute__((format(printf, 2, 3))) static void fail(const command *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: ", rp.script, c->line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    rp.failures++;
}

/* ---- keys ---- */

static const struct {
    const char *name;
    int key, ch;
} named[] = {
    {"enter", KEY_ENTER, 0},         {"return", KEY_ENTER, 0},        {"esc", KEY_ESCAPE, 0},  {"escape", KEY_ESCAPE, 0},
    {"tab", KEY_TAB, 0},             {"space", KEY_SPACE, ' '},       {"backspace", KEY_BACKSPACE, 0},
    {"delete", KEY_DELETE, 0},       {"del", KEY_DELETE, 0},          {"insert", KEY_INSERT, 0},
    {"up", KEY_UP, 0},               {"down", KEY_DOWN, 0},           {"left", KEY_LEFT, 0},   {"right", KEY_RIGHT, 0},
    {"home", KEY_HOME, 0},           {"end", KEY_END, 0},             {"pageup", KEY_PAGE_UP, 0},
    {"pagedown", KEY_PAGE_DOWN, 0},
};

static const char plain[] = "`1234567890-=[]\\;',./";
static const char shifted[] = "~!@#$%^&*()_+{}|:\"<>?";
static const int plain_keys[] = {KEY_GRAVE,        KEY_ONE,           KEY_TWO,       KEY_THREE,     KEY_FOUR,
                                 KEY_FIVE,         KEY_SIX,           KEY_SEVEN,     KEY_EIGHT,     KEY_NINE,
                                 KEY_ZERO,         KEY_MINUS,         KEY_EQUAL,     KEY_LEFT_BRACKET,
                                 KEY_RIGHT_BRACKET, KEY_BACKSLASH,    KEY_SEMICOLON, KEY_APOSTROPHE, KEY_COMMA,
                                 KEY_PERIOD,       KEY_SLASH};

/* the key, its modifiers and the character GLFW would type with it (0 none: Ctrl, Alt, Super or no printable) */
static int parse_key(const char *spec, int *key, int *mods, int *ch)
{
    static const struct {
        const char *prefix;
        int mod;
    } prefixes[] = {{"ctrl+", MOD_CTRL}, {"shift+", MOD_SHIFT}, {"alt+", MOD_ALT}, {"super+", MOD_SUPER}};
    *key = *mods = *ch = 0;
    for (int again = 1; again;) {
        again = 0;
        for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++) {
            size_t n = strlen(prefixes[i].prefix);
            if (!strncasecmp(spec, prefixes[i].prefix, n) && spec[n]) spec += n, *mods |= prefixes[i].mod, again = 1;
        }
    }
    int c = 0, f = 0;
    char extra;
    if (strlen(spec) == 1) {
        c = (unsigned char)*spec;
        const char *at;
        if (c >= 'a' && c <= 'z') *key = KEY_A + c - 'a', c = *mods & MOD_SHIFT ? c - 'a' + 'A' : c;
        else if (c >= 'A' && c <= 'Z') *key = KEY_A + c - 'A', *mods |= MOD_SHIFT;
        else if ((at = strchr(plain, c))) {
            *key = plain_keys[at - plain];
            if (*mods & MOD_SHIFT) c = shifted[at - plain];
        } else if ((at = strchr(shifted, c))) *key = plain_keys[at - shifted], *mods |= MOD_SHIFT;
        else return -1;
    } else if ((*spec == 'f' || *spec == 'F') && sscanf(spec + 1, "%d%c", &f, &extra) == 1 && f >= 1 && f <= 12) {
        *key = KEY_F1 + f - 1;
    } else {
        for (size_t i = 0; i < sizeof named / sizeof *named && !*key; i++)
            if (!strcasecmp(spec, named[i].name)) *key = named[i].key, c = named[i].ch;
        if (!*key) return -1;
    }
    *ch = *mods & (MOD_CTRL | MOD_ALT | MOD_SUPER) ? 0 : c;
    return 0;
}

/* ---- targets ---- */

static Vector2 centre(Rectangle r) { return (Vector2){r.x + r.width / 2, r.y + r.height / 2}; }

static int parse_xy(const char *s, Vector2 *at)
{
    int used = 0;
    if (sscanf(s, "%f %f %n", &at->x, &at->y, &used) != 2 || s[used]) return -1;
    if (at->x < 0) at->x += ui.w;
    if (at->y < 0) at->y += ui.h;
    return 0;
}

static int target(const command *c, Vector2 *at)
{
    const char *a = c->arg;
    Rectangle r;
    if (!strncmp(a, "row ", 4)) {
        const char *name = a + 4;
        for (int v = 0; v < br.view.n; v++) {
            if (strcmp(browser_item(v)->e.name, name)) continue;
            if (ui_clickable_rect(ui_id(name, (int)ui_id("row", 0)), &r) || ui_clickable_rect(ui_id(name, (int)ui_id("tile", 0)), &r))
                return *at = centre(r), 0;
            fail(c, "row \"%s\" is not on screen", name);
            return -1;
        }
        fail(c, "no row \"%s\" in %s", name, br.uri ? br.uri : "(no folder)");
        return -1;
    }
    if (!strncmp(a, "place ", 6)) {
        const char *label = a + 6;
        unsigned id = 0;
        for (int i = 0; i < app.places.n && !id; i++)
            if (!strcmp(app.places.v[i].label, label)) id = ui_id("place", i);
        for (int i = 0; i < settings.favorites.n && !id; i++) {
            char *name = fr_uri_name(settings.favorites.v[i]);
            if (!strcmp(*name ? name : "/", label)) id = ui_id("favorite", i);
            free(name);
        }
        if (!id) fail(c, "no place \"%s\"", label);
        else if (!ui_clickable_rect(id, &r)) fail(c, "place \"%s\" is not on screen", label);
        else return *at = centre(r), 0;
        return -1;
    }
    if (parse_xy(a, at)) {
        fail(c, "not a target: %s", a);
        return -1;
    }
    return 0;
}

/* ---- expectations ---- */

static char *resolve(const char *path)
{
    if (*path == '/') return fr_strdup(path);
    if (!strcmp(path, ".")) return fr_strdup(rp.base);
    return fr_format("%s/%s", rp.base, path);
}

static void expect(const command *c)
{
    const char *a = c->arg, *value = a + strcspn(a, " ");
    size_t sl = (size_t)(value - a);
    while (*value == ' ') value++;
#define IS(word) (sl == strlen(word) && !strncmp(a, word, sl))
    if (IS("folder")) {
        char *want = resolve(value), *real = realpath(want, NULL);
        const char *got = br.uri ? fr_uri_local(br.uri) : NULL;
        if (!got || strcmp(got, real ? real : want)) fail(c, "expected folder %s, got %s", real ? real : want, got ? got : br.uri ? br.uri : "(none)");
        free(want), free(real);
    } else if (IS("cursor")) {
        fr_item *it = browser_item(br.cursor);
        if (!it || strcmp(it->e.name, value)) fail(c, "expected the cursor on \"%s\", got %s%s%s", value, it ? "\"" : "", it ? it->e.name : "none", it ? "\"" : "");
    } else if (IS("selected")) {
        if (br.nselected != atoi(value)) fail(c, "expected %d selected, got %d", atoi(value), br.nselected);
    } else if (IS("exists") || IS("missing")) {
        char *path = resolve(value);
        struct stat st;
        int there = !lstat(path, &st), want = IS("exists");
        if (there != want) fail(c, "expected %s to %s", path, want ? "exist" : "be missing");
        free(path);
    } else if (IS("mode")) {
        static const char *modes[] = {"NORMAL", "VISUAL", "INSERT"};
        const char *got = modes[keys_mode()];
        if (strcasecmp(got, value)) fail(c, "expected mode %s, got %s", value, got);
    } else if (IS("clipboard")) {
        char *got = plat_clipboard();
        if (strcmp(got ? got : "", value)) fail(c, "expected the clipboard \"%s\", got \"%s\"", value, got ? got : "");
        free(got);
    } else if (IS("layout")) {
        static const char *layouts[] = {"details", "columns", "grid"};
        const char *got = layouts[app.layout];
        if (strcasecmp(got, value)) fail(c, "expected layout %s, got %s", value, got);
    } else if (IS("toast")) {
        const char *got = ui_toast_shown();
        if (!*got || !strstr(got, value)) fail(c, "expected a toast with \"%s\", got \"%s\"", value, got);
    }
#undef IS
}

/* ---- running ---- */

static int jobs_busy(void)
{
    fr_job_status st[16];
    int n = fr_jobs_list(st, 16);
    for (int i = 0; i < n; i++)
        if (st[i].state != JOB_ASKING) return 1;
    return 0;
}

static void shot(const command *c)
{
    rlDrawRenderBatchActive();
    Image img = LoadImageFromScreen();
    if (!img.data || !ExportImage(img, c->arg)) fail(c, "could not save %s", c->arg);
    UnloadImage(img);
}

/* 1: input went in, let frames run; 0: done, go on; -1: try again next frame */
static int run(const command *c)
{
    const char *v = c->verb;
    if (!strcmp(v, "key")) {
        int key, mods, ch;
        parse_key(c->arg, &key, &mods, &ch);
        ui_push_key(key, mods);
        if (ch) ui_push_char(ch);
    } else if (!strcmp(v, "type")) {
        int len = (int)strlen(c->arg);
        for (int i = 0, cp; i < len;) {
            i = fr_utf8_decode(c->arg, len, i, &cp);
            ui_push_char(cp);
        }
    } else if (!strcmp(v, "click") || !strcmp(v, "rclick") || !strcmp(v, "dclick") || !strcmp(v, "move")) {
        int right = v[0] == 'r', presses = v[0] == 'd' ? 2 : v[0] == 'm' ? 0 : 1;
        if (presses && !right && fr_now() - rp.pressed_at < CLICK_GAP_S) return -1;
        Vector2 at;
        if (target(c, &at)) return 0;
        rp.pointer = at;
        ui.dirty = 1;
        for (int i = 0; i < presses; i++) {
            ui_push_mouse(right ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT, 1);
            ui_push_mouse(right ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT, 0);
        }
        if (presses && !right) rp.pressed_at = fr_now();
    } else if (!strcmp(v, "wheel")) ui_push_wheel(0, strtof(c->arg, NULL));
    else if (!strcmp(v, "wait")) rp.wait = atoi(c->arg);
    else if (!strcmp(v, "quit")) app.quit = 1;
    else if (!strcmp(v, "expect")) {
        expect(c);
        return 0;
    } else rp.shot = c;
    return 1;
}

/* before EndDrawing: the frame just drawn is what a shot asked for */
void replay_capture(void)
{
    if (!rp.shot) return;
    shot(rp.shot);
    rp.shot = NULL;
}

int replay_frame(void)
{
    int idle = !ui.dirty && !ui_events_pending() && !jobs_busy() && !thumbs_busy();
    ui.dirty = 1;
    if (rp.wait > 0) return rp.wait--, 1;
    if (!idle && ++rp.settling < IDLE_FRAMES_MAX) return 1;
    rp.settling = 0;
    while (rp.at < rp.cmds.n && !app.quit) {
        int r = run(&rp.cmds.v[rp.at]);
        if (r < 0) return 1;
        rp.at++;
        if (r > 0) return 1;
    }
    return 0;
}

int replay_status(void)
{
    if (rp.at < rp.cmds.n) {
        fprintf(stderr, "%s:%d: the window closed before this line\n", rp.script, rp.cmds.v[rp.at].line);
        rp.failures++;
    }
    return rp.failures ? 1 : 0;
}

/* ---- the script ---- */

static const char *check(const char *verb, const char *arg)
{
    int i, n = 0;
    float f;
    Vector2 xy;
    if (!strcmp(verb, "key")) return parse_key(arg, &i, &i, &i) ? "not a key" : NULL;
    if (!strcmp(verb, "type")) return !*arg ? "nothing to type" : strlen(arg) > TYPE_MAX ? "too long to type at once" : NULL;
    if (!strcmp(verb, "click") || !strcmp(verb, "rclick") || !strcmp(verb, "dclick") || !strcmp(verb, "move"))
        return !strncmp(arg, "row ", 4) && arg[4] ? NULL : !strncmp(arg, "place ", 6) && arg[6] ? NULL
               : !parse_xy(arg, &xy) ? NULL : "the target is X Y, row NAME or place LABEL";
    if (!strcmp(verb, "wheel")) return sscanf(arg, "%f %n", &f, &n) == 1 && !arg[n] ? NULL : "wheel takes a number";
    if (!strcmp(verb, "wait")) return sscanf(arg, "%d %n", &i, &n) == 1 && !arg[n] && i >= 0 ? NULL : "wait takes a frame count";
    if (!strcmp(verb, "shot")) return *arg ? NULL : "shot takes a file";
    if (!strcmp(verb, "quit")) return *arg ? "quit takes nothing" : NULL;
    if (!strcmp(verb, "expect")) {
        static const char *subjects[] = {"folder", "cursor", "selected", "exists", "missing", "mode", "clipboard", "layout", "toast"};
        size_t sl = strcspn(arg, " ");
        for (size_t k = 0; k < sizeof subjects / sizeof *subjects; k++)
            if (sl == strlen(subjects[k]) && !strncmp(arg, subjects[k], sl))
                return arg[sl] || !strcmp(subjects[k], "clipboard") ? NULL : "expect what?";
        return "expect folder, cursor, selected, exists, missing, mode, clipboard, layout or toast";
    }
    return "unknown command";
}

int replay_load(const char *script, const char *folder)
{
    FILE *f = fopen(script, "r");
    if (!f) {
        fprintf(stderr, "ferret: %s: %s\n", script, strerror(errno));
        return -1;
    }
    rp.script = script;
    char line[4096];
    int no = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        no++;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
        char *s = line + strspn(line, " \t");
        if (!*s || *s == '#') continue;
        char *arg = s + strcspn(s, " \t");
        if (*arg) *arg++ = 0;
        arg += strspn(arg, " \t");
        const char *why = check(s, arg);
        if (why) {
            fprintf(stderr, "%s:%d: %s\n", script, no, why);
            bad++;
        } else FR_PUSH(rp.cmds, ((command){no, fr_strdup(s), fr_strdup(arg)}));
    }
    fclose(f);
    if (bad) return -1;
    rp.base = folder ? realpath(folder, NULL) : getcwd(NULL, 0);
    if (!rp.base) {
        fprintf(stderr, "ferret: %s: %s\n", folder ? folder : ".", strerror(errno));
        return -1;
    }
    rp.on = 1;
    return 0;
}

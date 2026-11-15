/* Nord (nordtheme.com): Polar Night surfaces, Snow Storm text, Frost accents, Aurora for file kinds and states;
 * the light scheme turns the first two around. On the desktop the colours follow ~/.local/state/omarchy/current/theme/colors.toml when it is there (the roles
 * folio and hyprlock read); the [theme] table of settings.toml overrides both. */
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

fr_theme theme;

enum { R_ACCENT, R_BACKGROUND, R_DARK_BACKGROUND, R_LIGHTER_BACKGROUND, R_FOREGROUND, R_BRIGHT_FOREGROUND, R_RED,
       R_YELLOW, R_GREEN, R_BLUE, R_MAGENTA, R_CYAN, R_ORANGE };
const char *const theme_roles[THEME_ROLES] = {"accent", "background", "dark_background", "lighter_background",
                                              "foreground", "bright_foreground", "red", "yellow", "green", "blue",
                                              "magenta", "cyan", "orange"};

static Color hex(unsigned v) { return (Color){(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v, 255}; }

static int parse_hex(const char *s, Color *out)
{
    while (*s == ' ' || *s == '"' || *s == '\'') s++;
    if (*s == '#') s++;
    unsigned v;
    if (sscanf(s, "%6x", &v) != 1) return 0;
    *out = hex(v);
    return 1;
}

#if defined(__linux__) && !defined(__ANDROID__)
/* Rec. 709 luma of the gamma-encoded channels (not WCAG's linear luminance): enough to tell a light palette from a
 * dark one */
static float luminance(Color c)
{
    return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
}

/* the roles the desktop's colors.toml sets into c and set[]; 0 when there is none */
static int read_desktop(Color *c, int *set)
{
    const char *home = getenv("HOME");
    if (!home) return 0;
    char *path = fr_format("%s/.local/state/omarchy/current/theme/colors.toml", home);
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        char *end = eq;
        while (end > line && (end[-1] == ' ' || end[-1] == '\t')) end--;
        for (int i = 0; i < THEME_ROLES; i++)
            if ((size_t)(end - line) == strlen(theme_roles[i]) && !strncmp(line, theme_roles[i], (size_t)(end - line)))
                set[i] |= parse_hex(eq + 1, &c[i]);
    }
    fclose(f);
    return 1;
}
#endif

int theme_desktop_light(void)
{
#if defined(__linux__) && !defined(__ANDROID__)
    Color c[THEME_ROLES] = {0};
    int set[THEME_ROLES] = {0};
    if (read_desktop(c, set) && set[R_BACKGROUND]) return luminance(c[R_BACKGROUND]) > 0.5f;
#endif
    return -1;
}

void theme_load(char *const *overrides, int light)
{
    /* Nord light: Snow Storm backgrounds, Polar Night text; the accent is Frost's nord10, which Nord's own light
     * ports use, since nord8 on Snow Storm is too faint for text */
    Color c[THEME_ROLES] = {[R_ACCENT] = hex(0x88C0D0),     [R_BACKGROUND] = hex(0x2E3440),
                            [R_DARK_BACKGROUND] = hex(0x242933), [R_LIGHTER_BACKGROUND] = hex(0x3B4252),
                            [R_FOREGROUND] = hex(0xD8DEE9), [R_BRIGHT_FOREGROUND] = hex(0xECEFF4),
                            [R_RED] = hex(0xBF616A),        [R_YELLOW] = hex(0xEBCB8B),
                            [R_GREEN] = hex(0xA3BE8C),      [R_BLUE] = hex(0x81A1C1),
                            [R_MAGENTA] = hex(0xB48EAD),    [R_CYAN] = hex(0x8FBCBB),
                            [R_ORANGE] = hex(0xD08770)};
    if (light) {
        c[R_ACCENT] = hex(0x5E81AC);
        c[R_BACKGROUND] = hex(0xECEFF4);
        c[R_DARK_BACKGROUND] = hex(0xE5E9F0);
        c[R_LIGHTER_BACKGROUND] = hex(0xF8F9FB);
        c[R_FOREGROUND] = hex(0x434C5E);
        c[R_BRIGHT_FOREGROUND] = hex(0x2E3440);
        c[R_BLUE] = hex(0x5E81AC);
    }
#if defined(__linux__) && !defined(__ANDROID__)
    /* the desktop's palette, when it is as light or dark as asked */
    Color d[THEME_ROLES];
    int set[THEME_ROLES] = {0};
    memcpy(d, c, sizeof d);
    if (read_desktop(d, set) && (!set[R_BACKGROUND] || (luminance(d[R_BACKGROUND]) > 0.5f) == !!light)) memcpy(c, d, sizeof c);
#endif
    for (int i = 0; overrides && i < THEME_ROLES; i++)
        if (overrides[i]) parse_hex(overrides[i], &c[i]);
    Color bg = c[R_BACKGROUND], dark = c[R_DARK_BACKGROUND], raised = c[R_LIGHTER_BACKGROUND], fg = c[R_FOREGROUND],
          bright = c[R_BRIGHT_FOREGROUND], accent = c[R_ACCENT], red = c[R_RED], yellow = c[R_YELLOW], blue = c[R_BLUE],
          magenta = c[R_MAGENTA], cyan = c[R_CYAN], orange = c[R_ORANGE], green = c[R_GREEN];
    /* nord3, the muted grey between Polar Night and Snow Storm; on light the borders take nord4 */
    Color nord3 = hex(0x4C566A), mute = light ? hex(0xD8DEE9) : nord3;
    /* Aurora is made for Polar Night: on Snow Storm the icons take some of the text colour to keep their contrast */
    float deepen = light ? 0.25f : 0;
    theme.bg = bg;
    theme.surface = dark;
    theme.surface_hi = raised;
    theme.border = light ? ui_mix(mute, nord3, 0.2f) : ui_mix(raised, nord3, 0.5f);
    theme.text = bright;
    theme.text_dim = fg;
    theme.text_faint = light ? ui_mix(fg, mute, 0.45f) : ui_mix(fg, nord3, 0.55f);
    theme.accent = accent;
    theme.on_accent = bg;
    theme.danger = red;
    theme.warning = ui_mix(yellow, bright, deepen);
    theme.success = ui_mix(green, bright, deepen);
    theme.hover = ui_alpha(light ? bright : fg, 0.06f);
    theme.pressed = ui_alpha(light ? bright : fg, 0.12f);
    theme.selection = ui_alpha(accent, 0.18f);
    theme.scrim = light ? ui_alpha(bright, 0.35f) : ui_alpha(dark, 0.6f);
    theme.folder = accent;
    theme.image = ui_mix(magenta, bright, deepen);
    theme.video = ui_mix(orange, bright, deepen);
    theme.audio = ui_mix(green, bright, deepen);
    theme.archive = ui_mix(yellow, bright, deepen);
    theme.document = blue;
    theme.sheet = ui_mix(green, bright, deepen);
    theme.slides = ui_mix(orange, bright, deepen);
    theme.code = ui_mix(cyan, bright, deepen);
    theme.other = light ? ui_mix(fg, mute, 0.3f) : ui_mix(fg, nord3, 0.3f);
}

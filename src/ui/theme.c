/* Nord (nordtheme.com): Polar Night surfaces, Snow Storm text, Frost accents, Aurora for file kinds and states.
 * On the desktop the colours follow ~/.local/state/omarchy/current/theme/colors.toml when it is there (the roles
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
static void read_desktop(Color *c)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char *path = fr_format("%s/.local/state/omarchy/current/theme/colors.toml", home);
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        char *end = eq;
        while (end > line && (end[-1] == ' ' || end[-1] == '\t')) end--;
        for (int i = 0; i < THEME_ROLES; i++)
            if ((size_t)(end - line) == strlen(theme_roles[i]) && !strncmp(line, theme_roles[i], (size_t)(end - line)))
                parse_hex(eq + 1, &c[i]);
    }
    fclose(f);
}
#endif

void theme_load(char *const *overrides)
{
    Color c[THEME_ROLES] = {[R_ACCENT] = hex(0x88C0D0),     [R_BACKGROUND] = hex(0x2E3440),
                            [R_DARK_BACKGROUND] = hex(0x242933), [R_LIGHTER_BACKGROUND] = hex(0x3B4252),
                            [R_FOREGROUND] = hex(0xD8DEE9), [R_BRIGHT_FOREGROUND] = hex(0xECEFF4),
                            [R_RED] = hex(0xBF616A),        [R_YELLOW] = hex(0xEBCB8B),
                            [R_GREEN] = hex(0xA3BE8C),      [R_BLUE] = hex(0x81A1C1),
                            [R_MAGENTA] = hex(0xB48EAD),    [R_CYAN] = hex(0x8FBCBB),
                            [R_ORANGE] = hex(0xD08770)};
#if defined(__linux__) && !defined(__ANDROID__)
    read_desktop(c);
#endif
    for (int i = 0; overrides && i < THEME_ROLES; i++)
        if (overrides[i]) parse_hex(overrides[i], &c[i]);
    Color nord0 = c[R_BACKGROUND], dark = c[R_DARK_BACKGROUND], nord1 = c[R_LIGHTER_BACKGROUND], nord4 = c[R_FOREGROUND],
          nord6 = c[R_BRIGHT_FOREGROUND], nord8 = c[R_ACCENT], red = c[R_RED], yellow = c[R_YELLOW], blue = c[R_BLUE],
          magenta = c[R_MAGENTA], cyan = c[R_CYAN], orange = c[R_ORANGE], green = c[R_GREEN], nord3 = hex(0x4C566A);
    theme.bg = nord0;
    theme.surface = dark;
    theme.surface_hi = nord1;
    theme.border = ui_mix(nord1, nord3, 0.5f);
    theme.text = nord6;
    theme.text_dim = nord4;
    theme.text_faint = ui_mix(nord4, nord3, 0.55f);
    theme.accent = nord8;
    theme.on_accent = nord0;
    theme.danger = red;
    theme.warning = yellow;
    theme.success = green;
    theme.hover = ui_alpha(nord4, 0.06f);
    theme.pressed = ui_alpha(nord4, 0.12f);
    theme.selection = ui_alpha(nord8, 0.18f);
    theme.scrim = ui_alpha(dark, 0.6f);
    theme.folder = nord8;
    theme.image = magenta;
    theme.video = orange;
    theme.audio = green;
    theme.archive = yellow;
    theme.document = blue;
    theme.sheet = green;
    theme.slides = orange;
    theme.code = cyan;
    theme.other = ui_mix(nord4, nord3, 0.3f);
}

/* Nord (nordtheme.com): Polar Night surfaces, Snow Storm text, Frost accents, Aurora for file kinds and states.
 * On the desktop the colours follow ~/.local/state/omarchy/current/theme/colors.toml when it is there (the roles
 * folio and hyprlock read). */
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

fr_theme theme;

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
static void read_desktop(Color *bg, Color *dark, Color *lighter, Color *fg, Color *bright, Color *accent, Color *red,
                         Color *yellow, Color *blue, Color *magenta, Color *cyan, Color *orange)
{
    const char *home = getenv("HOME");
    if (!home) return;
    char *path = fr_format("%s/.local/state/omarchy/current/theme/colors.toml", home);
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return;
    struct {
        const char *key;
        Color *dst;
    } keys[] = {{"background", bg}, {"dark_background", dark}, {"lighter_background", lighter},
                {"foreground", fg},  {"bright_foreground", bright}, {"accent", accent},
                {"red", red},        {"yellow", yellow},            {"blue", blue},
                {"magenta", magenta}, {"cyan", cyan},               {"orange", orange}};
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        char *end = eq;
        while (end > line && (end[-1] == ' ' || end[-1] == '\t')) end--;
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
            if ((size_t)(end - line) == strlen(keys[i].key) && !strncmp(line, keys[i].key, (size_t)(end - line)))
                parse_hex(eq + 1, keys[i].dst);
    }
    fclose(f);
}
#endif

void theme_load(void)
{
    Color nord0 = hex(0x2E3440), dark = hex(0x242933), nord1 = hex(0x3B4252), nord4 = hex(0xD8DEE9),
          nord6 = hex(0xECEFF4), nord8 = hex(0x88C0D0), red = hex(0xBF616A), yellow = hex(0xEBCB8B),
          blue = hex(0x81A1C1), magenta = hex(0xB48EAD), cyan = hex(0x8FBCBB), orange = hex(0xD08770),
          green = hex(0xA3BE8C), nord3 = hex(0x4C566A);
#if defined(__linux__) && !defined(__ANDROID__)
    read_desktop(&nord0, &dark, &nord1, &nord4, &nord6, &nord8, &red, &yellow, &blue, &magenta, &cyan, &orange);
#endif
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

#include "ui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char fr_font_regular[], fr_font_semibold[];
extern const int fr_font_regular_size, fr_font_semibold_size;

/* Fonts are rasterised at the exact pixel size they are drawn at; a scale change reloads them. */
typedef struct {
    int weight, px;
    Font font;
} cached_font;

static cached_font fonts[16];
static int nfonts;
static int *codepoints, ncodepoints;

/* Latin with its extensions, Greek, Cyrillic, punctuation and currency: what Inter covers that names use most. */
static void build_codepoints(void)
{
    static const int ranges[][2] = {{0x20, 0x7E},     {0xA0, 0x24F},   {0x370, 0x3FF}, {0x400, 0x4FF},
                                    {0x1E00, 0x1EFF}, {0x2000, 0x206F}, {0x20A0, 0x20C0}, {0x2100, 0x214F}};
    ncodepoints = 0;
    for (size_t r = 0; r < sizeof ranges / sizeof *ranges; r++) ncodepoints += ranges[r][1] - ranges[r][0] + 1;
    codepoints = fr_realloc(NULL, sizeof *codepoints * (size_t)ncodepoints);
    int k = 0;
    for (size_t r = 0; r < sizeof ranges / sizeof *ranges; r++)
        for (int c = ranges[r][0]; c <= ranges[r][1]; c++) codepoints[k++] = c;
}

void ui_fonts_reload(void)
{
    for (int i = 0; i < nfonts; i++) UnloadFont(fonts[i].font);
    nfonts = 0;
}

Font ui_font(int weight, float size_dp)
{
    int px = (int)lroundf(DP(size_dp) * (ui.text_scale > 0 ? ui.text_scale : 1));
    if (px < 6) px = 6;
    for (int i = 0; i < nfonts; i++)
        if (fonts[i].weight == weight && fonts[i].px == px) return fonts[i].font;
    if (!codepoints) build_codepoints();
    if (nfonts == (int)(sizeof fonts / sizeof *fonts)) UnloadFont(fonts[0].font), memmove(fonts, fonts + 1, sizeof *fonts * (size_t)--nfonts);
    const unsigned char *data = weight == FONT_SEMIBOLD ? fr_font_semibold : fr_font_regular;
    int size = weight == FONT_SEMIBOLD ? fr_font_semibold_size : fr_font_regular_size;
    Font f = LoadFontFromMemory(".ttf", data, size, px, codepoints, ncodepoints);
    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    fonts[nfonts++] = (cached_font){weight, px, f};
    return f;
}

float ui_line_height(float size_dp) { return roundf(DP(size_dp) * (ui.text_scale > 0 ? ui.text_scale : 1) * 1.3f); }

float ui_text_width_n(const char *s, int n, float size_dp, int weight)
{
    Font f = ui_font(weight, size_dp);
    float w = 0;
    for (int i = 0; i < n;) {
        int cp;
        i = fr_utf8_decode(s, n, i, &cp);
        int g = GetGlyphIndex(f, cp);
        w += f.glyphs[g].advanceX ? (float)f.glyphs[g].advanceX : f.recs[g].width + (float)f.glyphs[g].offsetX;
    }
    return w;
}

float ui_text_width(const char *s, float size_dp, int weight) { return ui_text_width_n(s, (int)strlen(s), size_dp, weight); }

static void draw_n(const char *s, int n, float x, float y, float size_dp, int weight, Color c)
{
    Font f = ui_font(weight, size_dp);
    x = roundf(x), y = roundf(y);
    for (int i = 0; i < n;) {
        int cp;
        i = fr_utf8_decode(s, n, i, &cp);
        int g = GetGlyphIndex(f, cp);
        if (cp != ' ' && cp != '\t') DrawTextCodepoint(f, cp, (Vector2){x, y}, (float)f.baseSize, c);
        x += f.glyphs[g].advanceX ? (float)f.glyphs[g].advanceX : f.recs[g].width + (float)f.glyphs[g].offsetX;
    }
}

void ui_text(const char *s, float x, float y, float size_dp, int weight, Color c)
{
    draw_n(s, (int)strlen(s), x, y, size_dp, weight, c);
}

/* the longest prefix of s[0..n) that fits in w */
static int fit(const char *s, int n, float w, float size_dp, int weight)
{
    Font f = ui_font(weight, size_dp);
    float x = 0;
    for (int i = 0; i < n;) {
        int cp, next = fr_utf8_decode(s, n, i, &cp), g = GetGlyphIndex(f, cp);
        x += f.glyphs[g].advanceX ? (float)f.glyphs[g].advanceX : f.recs[g].width + (float)f.glyphs[g].offsetX;
        if (x > w) return i;
        i = next;
    }
    return n;
}

void ui_text_box(const char *s, Rectangle r, float size_dp, int weight, Color c, int align, int middle)
{
    Font f = ui_font(weight, size_dp);
    float y = r.y + (r.height - (float)f.baseSize) / 2;
    int n = (int)strlen(s);
    float w = ui_text_width_n(s, n, size_dp, weight);
    if (w <= r.width) {
        float x = align == ALIGN_CENTER ? r.x + (r.width - w) / 2 : align == ALIGN_RIGHT ? r.x + r.width - w : r.x;
        draw_n(s, n, x, y, size_dp, weight, c);
        return;
    }
    float dots = ui_text_width("...", size_dp, weight), room = r.width - dots;
    if (room <= 0) return;
    if (!middle) {
        int k = fit(s, n, room, size_dp, weight);
        draw_n(s, k, r.x, y, size_dp, weight, c);
        draw_n("...", 3, r.x + ui_text_width_n(s, k, size_dp, weight), y, size_dp, weight, c);
        return;
    }
    /* keep the end (an extension, a number) and as much of the start as fits */
    int tail = n;
    float tail_w = 0;
    while (tail > 0) {
        int p = fr_utf8_prev(s, tail);
        float tw = ui_text_width_n(s + p, n - p, size_dp, weight);
        if (tw > room * 0.4f || n - p > 12) break;
        tail = p, tail_w = tw;
    }
    int head = fit(s, tail, room - tail_w, size_dp, weight);
    float hw = ui_text_width_n(s, head, size_dp, weight);
    draw_n(s, head, r.x, y, size_dp, weight, c);
    draw_n("...", 3, r.x + hw, y, size_dp, weight, c);
    draw_n(s + tail, n - tail, r.x + hw + dots, y, size_dp, weight, c);
}

int ui_text_wrapped(const char *s, Rectangle r, float size_dp, int weight, Color c, int draw)
{
    float lh = ui_line_height(size_dp);
    int lines = 0, n = (int)strlen(s), i = 0;
    while (i < n) {
        int end = i;
        while (end < n && s[end] != '\n') end++;
        int line_start = i;
        do {
            int k = line_start + fit(s + line_start, end - line_start, r.width, size_dp, weight);
            if (k < end) {
                int sp = k;
                while (sp > line_start && s[sp] != ' ') sp--;
                if (sp > line_start) k = sp;
                else if (k == line_start) k = fr_utf8_next(s, end, line_start);
            }
            if (draw) draw_n(s + line_start, k - line_start, r.x, r.y + lines * lh, size_dp, weight, c);
            lines++;
            line_start = k;
            while (line_start < end && s[line_start] == ' ') line_start++;
        } while (line_start < end);
        i = end + 1;
        if (end == n) break;
    }
    return lines;
}

void ui_rect(Rectangle r, Color c) { DrawRectangleRec(r, c); }

void ui_round(Rectangle r, float radius_dp, Color c)
{
    float m = fminf(r.width, r.height);
    if (m <= 0) return;
    DrawRectangleRounded(r, fminf(1, 2 * DP(radius_dp) / m), 12, c);
}

void ui_round_line(Rectangle r, float radius_dp, Color c)
{
    float m = fminf(r.width, r.height);
    if (m <= 0) return;
    DrawRectangleRoundedLinesEx(r, fminf(1, 2 * DP(radius_dp) / m), 12, fmaxf(1, roundf(DP(1))), c);
}

void ui_hline(float x, float y, float w, Color c) { DrawRectangleRec((Rectangle){x, roundf(y), w, fmaxf(1, roundf(DP(1)))}, c); }

Color ui_mix(Color a, Color b, float t)
{
    return (Color){(unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                   (unsigned char)(a.b + (b.b - a.b) * t), (unsigned char)(a.a + (b.a - a.a) * t)};
}

Color ui_alpha(Color c, float a)
{
    c.a = (unsigned char)(255 * a);
    return c;
}

static Rectangle clips[8];
static int nclips;

static void apply_clip(void)
{
    if (!nclips) {
        EndScissorMode();
        return;
    }
    Rectangle r = clips[nclips - 1];
    BeginScissorMode((int)r.x, (int)r.y, (int)ceilf(r.width), (int)ceilf(r.height));
}

void ui_clip_begin(Rectangle r)
{
    if (nclips) {
        Rectangle p = clips[nclips - 1];
        float x0 = fmaxf(r.x, p.x), y0 = fmaxf(r.y, p.y);
        float x1 = fminf(r.x + r.width, p.x + p.width), y1 = fminf(r.y + r.height, p.y + p.height);
        r = (Rectangle){x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0)};
    }
    if (nclips < 8) clips[nclips++] = r;
    apply_clip();
}

void ui_clip_end(void)
{
    if (nclips) nclips--;
    apply_clip();
}

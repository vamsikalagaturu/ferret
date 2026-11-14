/* Lucide icons drawn white by nanosvg at the pixel size they are shown at, then tinted when drawn. */
#include "icons.h"
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "nanosvg.h"
#include "nanosvgrast.h"
#pragma GCC diagnostic pop

typedef struct {
    const char *name;
    int px;
    Texture2D tex;
} cached_icon;

static struct {
    cached_icon *v;
    int n, cap;
} cache;
static NSVGrasterizer *rast;

static const fr_icon_data *find(const char *name)
{
    for (const fr_icon_data *d = fr_icon_files; d->name; d++)
        if (!strcmp(d->name, name)) return d;
    return NULL;
}

/* nanosvg knows no currentColor */
static char *whitened(const fr_icon_data *d)
{
    const char *from = "currentColor", *to = "#ffffff";
    size_t n = (size_t)*d->size, fl = strlen(from), tl = strlen(to);
    char *out = fr_realloc(NULL, n * 2 + 1), *o = out;
    for (size_t i = 0; i < n;) {
        if (i + fl <= n && !memcmp(d->data + i, from, fl)) memcpy(o, to, tl), o += tl, i += fl;
        else *o++ = (char)d->data[i++];
    }
    *o = 0;
    return out;
}

static Texture2D render(const fr_icon_data *d, int px)
{
    Texture2D none = {0};
    char *svg = whitened(d);
    NSVGimage *img = nsvgParse(svg, "px", 96);
    free(svg);
    if (!img) return none;
    if (!rast) rast = nsvgCreateRasterizer();
    unsigned char *pixels = fr_realloc(NULL, (size_t)px * px * 4);
    float s = (float)px / (img->width > 0 ? img->width : 24);
    nsvgRasterize(rast, img, 0, 0, s, pixels, px, px, px * 4);
    nsvgDelete(img);
    Image im = {pixels, px, px, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D tex = LoadTextureFromImage(im);
    free(pixels);
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    return tex;
}

void ui_icon(const char *name, Rectangle box, float size_dp, Color c)
{
    int px = (int)lroundf(DP(size_dp));
    if (px < 4) return;
    Texture2D tex = {0};
    for (int i = 0; i < cache.n && !tex.id; i++)
        if (cache.v[i].px == px && !strcmp(cache.v[i].name, name)) tex = cache.v[i].tex;
    if (!tex.id) {
        const fr_icon_data *d = find(name);
        if (!d) {
            fprintf(stderr, "ferret: no icon %s\n", name);
            d = find("file");
        }
        tex = render(d, px);
        FR_PUSH(cache, ((cached_icon){d->name, px, tex}));
    }
    float x = roundf(box.x + (box.width - (float)px) / 2), y = roundf(box.y + (box.height - (float)px) / 2);
    DrawTexture(tex, (int)x, (int)y, c);
}

void ui_icons_clear(void)
{
    for (int i = 0; i < cache.n; i++) UnloadTexture(cache.v[i].tex);
    cache.n = 0;
}

/* Thumbnails as textures: what the frame drew asks the worker (src/thumbs) for what it lacks, in drawing order,
 * between frames; finished ones are uploaded then. Kept in memory up to MAX_TEXTURES, least recently drawn out
 * first. Nothing here touches the browser's lists. */
#include "app.h"
#include "thumbs/thumbs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEXTURES 300
#define MAX_FAILED 2000
#define UPLOADS_PER_FRAME 12

typedef struct {
    char *path;
    unsigned hash;
    int64_t mtime;
    uint64_t size;
    int px;
    Texture2D tex; /* id 0: none can be made */
    unsigned used; /* the frame it was last drawn in */
} entry;

static struct {
    entry *v;
    int n, cap;
} cache;
static struct {
    fr_thumb_req *v;
    int n, cap;
} wants;
static char *root;
static size_t root_len;
static unsigned frame_no = 1;
static int textures;

static unsigned hash_of(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static entry *find(const char *path, unsigned hash, int64_t mtime, uint64_t size, int px)
{
    for (int i = 0; i < cache.n; i++) {
        entry *e = &cache.v[i];
        if (e->hash == hash && e->px == px && e->mtime == mtime && e->size == size && !strcmp(e->path, path)) return e;
    }
    return NULL;
}

void thumbs_start(void)
{
    root = fr_thumb_cache_root();
    root_len = strlen(root);
    fr_thumbs_start(root, plat_wake);
}

static void drop(int i)
{
    if (cache.v[i].tex.id) UnloadTexture(cache.v[i].tex), textures--;
    free(cache.v[i].path);
    cache.v[i] = cache.v[--cache.n];
}

void thumbs_stop(void)
{
    fr_thumbs_stop();
    while (cache.n) drop(cache.n - 1);
    for (int i = 0; i < wants.n; i++) free((char *)wants.v[i].path);
    wants.n = 0;
    free(root);
    root = NULL;
}

/* the least recently drawn, of those with a texture or of those without; never one drawn this frame */
static void evict(int with_texture)
{
    int oldest = -1;
    for (int i = 0; i < cache.n; i++)
        if (!!cache.v[i].tex.id == with_texture && cache.v[i].used != frame_no && (oldest < 0 || cache.v[i].used < cache.v[oldest].used))
            oldest = i;
    if (oldest >= 0) drop(oldest);
}

void thumbs_poll(void)
{
    if (!root) return;
    fr_thumbs_request(wants.v, wants.n);
    for (int i = 0; i < wants.n; i++) free((char *)wants.v[i].path);
    wants.n = 0;
    fr_thumb_done d;
    for (int k = 0; k < UPLOADS_PER_FRAME && fr_thumbs_take(&d); k++) {
        unsigned hash = hash_of(d.path);
        if (!find(d.path, hash, d.mtime, d.size, d.px)) {
            entry e = {d.path, hash, d.mtime, d.size, d.px, {0}, frame_no};
            if (d.im.rgba) {
                Image im = {d.im.rgba, d.im.w, d.im.h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                e.tex = LoadTextureFromImage(im);
                if (e.tex.id) SetTextureFilter(e.tex, TEXTURE_FILTER_BILINEAR), textures++;
            }
            FR_PUSH(cache, e);
            d.path = NULL;
        }
        fr_thumb_done_free(&d);
        ui.dirty = 1;
    }
    while (textures > MAX_TEXTURES) {
        int before = textures;
        evict(1);
        if (textures == before) break;
    }
    while (cache.n - textures > MAX_FAILED) {
        int before = cache.n;
        evict(0);
        if (cache.n == before) break;
    }
    frame_no++;
}

int thumbs_busy(void) { return root && (wants.n || fr_thumbs_busy()); }

/* px: the size class tex was made for */
static void draw_fitted(Texture2D tex, int px, Rectangle box)
{
    float s = fminf(box.width / tex.width, box.height / tex.height);
    /* an image smaller than its class is shown at its own size at most, a pixel a dp, not blown up */
    if (tex.width < px && tex.height < px) s = fminf(s, ui.scale);
    float w = roundf(tex.width * s), h = roundf(tex.height * s);
    Rectangle dst = {roundf(box.x + (box.width - w) / 2), roundf(box.y + (box.height - h) / 2), w, h};
    DrawTexturePro(tex, (Rectangle){0, 0, (float)tex.width, (float)tex.height}, dst, (Vector2){0, 0}, 0, WHITE);
}

int thumb_draw(const fr_item *it, Rectangle box)
{
    if (!root || it->e.is_dir || it->placeholder || !it->e.size || it->e.size > FR_THUMB_MAX_BYTES || !br.uri ||
        !fr_thumb_supported(it->e.name))
        return -1;
    char *uri = fr_uri_join(br.uri, it->e.name);
    const char *path = fr_uri_local(uri);
    /* the specification's rule: no thumbnails of thumbnails */
    if (!path || (!strncmp(path, root, root_len) && path[root_len] == '/')) {
        free(uri);
        return -1;
    }
    int px = fr_thumb_class(fmaxf(box.width, box.height)), drawn = 0;
    unsigned hash = hash_of(path);
    entry *e = find(path, hash, it->e.mtime, it->e.size, px);
    if (e) {
        e->used = frame_no;
        if (e->tex.id) draw_fitted(e->tex, px, box);
        free(uri);
        return e->tex.id ? 1 : -1;
    }
    int asked = 0;
    for (int i = 0; i < wants.n && !asked; i++) asked = wants.v[i].px == px && !strcmp(wants.v[i].path, path);
    if (!asked) FR_PUSH(wants, ((fr_thumb_req){fr_strdup(path), it->e.mtime, it->e.size, px}));
    /* another size of it meanwhile */
    for (int c = FR_THUMB_XLARGE; c >= FR_THUMB_NORMAL && !drawn; c /= 2) {
        entry *o = c == px ? NULL : find(path, hash, it->e.mtime, it->e.size, c);
        if (o && o->tex.id) o->used = frame_no, draw_fitted(o->tex, c, box), drawn = 1;
    }
    free(uri);
    return drawn;
}

/* Pictures at the size a preview shows them (the columns' preview pane, the properties' tile), where the
 * thumbnails stop at 512 px and at files on this device. The file is read through the VFS and decoded with the
 * thumbnails' decoder on a thread of its own; a few are kept, the least recently drawn going first. */
#include "app.h"
#include "jobs/async.h"
#include "thumbs/thumbs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 4
#define MAX_PX 2048
#define REMOTE_MAX_BYTES (32u << 20) /* read whole over the network first: kept small */
#define SETTLE_S 0.12 /* the cursor stays this long on a file before it is read: none for each file passed over */

typedef struct load {
    char *uri;
    uint64_t size;
    int px, cancel, rc;
    fr_thumb_image im;
} load;

typedef struct {
    char *uri;
    int64_t mtime;
    uint64_t size;
    int px;
    Texture2D tex;
    int failed;
    load *pending; /* on its way */
    unsigned used; /* the frame it was last drawn in */
    double asked_at;
} slot;

static slot slots[SLOTS];
static unsigned frame_no = 1;

static void slot_clear(slot *s)
{
    if (s->pending) __atomic_store_n(&s->pending->cancel, 1, __ATOMIC_RELAXED);
    if (s->tex.id) UnloadTexture(s->tex);
    free(s->uri);
    *s = (slot){0};
}

static void work(void *arg)
{
    load *l = arg;
    int err = 0;
    fr_stream *st = fr_vfs_open_read(l->uri, &err);
    if (!st) {
        l->rc = err ? err : -1;
        return;
    }
    size_t cap = (size_t)l->size + 1, n = 0;
    unsigned char *buf = malloc(cap);
    long got = 0;
    while (buf && n < cap && !__atomic_load_n(&l->cancel, __ATOMIC_RELAXED) && (got = st->read(st, buf + n, cap - n)) > 0) n += (size_t)got;
    st->close(st);
    /* a file grown past what its entry said is read again once its listing says so */
    if (!buf || got < 0 || n >= cap || __atomic_load_n(&l->cancel, __ATOMIC_RELAXED)) l->rc = -1;
    else l->rc = fr_thumb_decode(buf, n, l->px, &l->im);
    free(buf);
}

static void done(void *arg)
{
    load *l = arg;
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &slots[i];
        if (s->pending != l) continue;
        s->pending = NULL;
        if (!l->rc && l->im.rgba) {
            Image im = {l->im.rgba, l->im.w, l->im.h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            s->tex = LoadTextureFromImage(im);
            if (s->tex.id) SetTextureFilter(s->tex, TEXTURE_FILTER_BILINEAR);
        }
        s->failed = !s->tex.id;
        ui.dirty = 1;
    }
    fr_thumb_image_free(&l->im);
    free(l->uri);
    free(l);
}

static void draw_fitted(Texture2D tex, Rectangle box)
{
    float s = fminf(box.width / tex.width, box.height / tex.height);
    /* a picture smaller than the box is shown at a pixel a dp at most, not blown up */
    s = fminf(s, ui.scale);
    float w = roundf(tex.width * s), h = roundf(tex.height * s);
    Rectangle dst = {roundf(box.x + (box.width - w) / 2), roundf(box.y + (box.height - h) / 2), w, h};
    DrawTexturePro(tex, (Rectangle){0, 0, (float)tex.width, (float)tex.height}, dst, (Vector2){0, 0}, 0, WHITE);
}

int preview_image_draw(const char *uri, const fr_entry *e, Rectangle box)
{
    if (!uri) return -1;
    int local = fr_uri_local(uri) != NULL;
    if (e->is_dir || !e->size || e->size > (local ? FR_THUMB_MAX_BYTES : REMOTE_MAX_BYTES) || !fr_thumb_supported(e->name) ||
        box.width < 1 || box.height < 1)
        return -1;
    /* in steps of 256, so a resize does not read it again for every pixel */
    int px = ((int)ceilf(fmaxf(box.width, box.height)) + 255) / 256 * 256;
    if (px > MAX_PX) px = MAX_PX;
    slot *s = NULL, *lru = &slots[0];
    for (int i = 0; i < SLOTS; i++) {
        slot *c = &slots[i];
        if (c->uri && c->px == px && c->mtime == e->mtime && c->size == e->size && !strcmp(c->uri, uri)) s = c;
        if (c->used < lru->used) lru = c;
    }
    if (!s) {
        slot_clear(lru);
        s = lru;
        *s = (slot){.uri = fr_strdup(uri), .mtime = e->mtime, .size = e->size, .px = px, .asked_at = ui.now};
        ui_redraw_in(SETTLE_S);
    }
    s->used = frame_no;
    if (s->tex.id) {
        draw_fitted(s->tex, box);
        return 1;
    }
    return s->failed ? -1 : 0;
}

void preview_poll(void)
{
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &slots[i];
        if (!s->uri || s->tex.id || s->failed || s->pending || s->used != frame_no) continue;
        if (ui.now - s->asked_at < SETTLE_S) {
            ui_redraw_in(SETTLE_S - (ui.now - s->asked_at));
            continue;
        }
        load *l = fr_realloc(NULL, sizeof *l);
        *l = (load){.uri = fr_strdup(s->uri), .size = s->size, .px = s->px};
        s->pending = l;
        net_prepare(s->uri);
        fr_async_run(work, done, l);
    }
    frame_no++;
}

#ifndef FR_THUMBS_H
#define FR_THUMBS_H
/* Image thumbnails, made on a worker thread and kept on disk the way the freedesktop thumbnail specification
 * (0.9.0) keeps them: <root>/{normal,large,x-large}/md5(URI).png, with the source's URI, mtime and size in the
 * PNG's tEXt chunks, so a changed file gets a new thumbnail. The root is ferret's own, not the shared
 * ~/.cache/thumbnails. Decoding is stb_image's (PNG, JPEG, BMP, GIF's first frame, TGA, PSD, HDR) and qoi.h's,
 * the copies raylib vendors. No window: the window uploads the pixels itself. */
#include "core/util.h"
#include <stdint.h>

#define FR_THUMB_MAX_BYTES (40u << 20) /* bigger files get none */
#define FR_THUMB_MAX_PIXELS (64u << 20) /* nor do images with more pixels: their decode alone would need 4 B each */
enum { FR_THUMB_NORMAL = 128, FR_THUMB_LARGE = 256, FR_THUMB_XLARGE = 512 };

typedef struct {
    unsigned char *rgba; /* w * h * 4, malloc'd */
    int w, h;
} fr_thumb_image;
void fr_thumb_image_free(fr_thumb_image *im);

int fr_thumb_supported(const char *name); /* by the extension */
int fr_thumb_class(float px);             /* the smallest size class of at least px pixels, else the largest */

void fr_md5_hex(const void *data, size_t n, char out[33]);
/* "file://" and the path with every byte outside RFC 3986's unreserved and sub-delims, ':', '@' and '/'
 * percent-encoded: the URI the specification hashes */
char *fr_thumb_uri(const char *path);
char *fr_thumb_cache_root(void); /* $XDG_CACHE_HOME/ferret/thumbs, else ~/.cache/ferret/thumbs */
/* <root>/<class folder>/md5(fr_thumb_uri(path)).png */
char *fr_thumb_cache_path(const char *root, const char *path, int px);

/* w x h scaled to fit max x max, aspect kept, never enlarged and never below 1 x 1 */
void fr_thumb_fit(int w, int h, int max, int *ow, int *oh);
/* src (channels 3 or 4) averaged over the area each dst pixel covers into RGBA dst; alpha-weighted, so transparent
 * pixels do not darken their neighbours. Only shrinks. */
void fr_thumb_downscale(const unsigned char *src, int sw, int sh, int channels, unsigned char *dst, int dw, int dh);
/* a JPEG's EXIF Orientation (1 to 8, 1 as stored), 1 for anything else; stb_image does not apply it */
int fr_thumb_exif_orientation(const unsigned char *data, size_t n);
void fr_thumb_orient(fr_thumb_image *im, int orientation); /* turns or mirrors im as orientation asks */
/* an encoded image in memory to RGBA fitted in max_px, turned upright; 0, or -1 when it is not one or too big */
int fr_thumb_decode(const unsigned char *data, size_t n, int max_px, fr_thumb_image *out);
/* writes im as a PNG with Thumb::URI, Thumb::MTime and Thumb::Size, through a temporary file and a rename */
int fr_thumb_save(const char *cache_path, const fr_thumb_image *im, const char *uri, int64_t mtime, uint64_t size);
/* 0 with the thumbnail at cache_path when its text chunks match uri, mtime and size; -1 when missing or stale */
int fr_thumb_load(const char *cache_path, const char *uri, int64_t mtime, uint64_t size, fr_thumb_image *out);
/* the disk cache's thumbnail of the local file path, else a new one, saved there when root is not NULL */
int fr_thumb_make(const char *root, const char *path, int64_t mtime, uint64_t size, int px, fr_thumb_image *out);
void fr_thumb_round(fr_thumb_image *im); /* corners made transparent, antialiased, radius 6% of the short side */

/* ---- the worker ---- */
typedef struct {
    const char *path;
    int64_t mtime;
    uint64_t size;
    int px;
} fr_thumb_req;

typedef struct {
    char *path;
    int64_t mtime;
    uint64_t size;
    int px;
    fr_thumb_image im; /* rgba NULL: none can be made */
} fr_thumb_done;

/* root: the disk cache (copied), NULL for none; wake: from the worker when a thumbnail is ready */
void fr_thumbs_start(const char *root, void (*wake)(void));
void fr_thumbs_stop(void);
/* what to make, most wanted first, replacing the last call's list: what is not in it any more is dropped, unless
 * the worker already started on it. Requests already done and not taken yet are left out. */
void fr_thumbs_request(const fr_thumb_req *reqs, int n);
int fr_thumbs_take(fr_thumb_done *out); /* 1 with a finished thumbnail (the caller's), 0 none */
void fr_thumb_done_free(fr_thumb_done *d);
int fr_thumbs_busy(void); /* something requested, being made or not taken */
#endif

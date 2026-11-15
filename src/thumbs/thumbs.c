#define _GNU_SOURCE
#include "thumbs.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* raylib compiles its own copies with only some formats; these are static, so the two never meet */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_ONLY_PSD
#define STBI_ONLY_HDR
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
/* qoi.h has no static mode: renamed, or they would clash with raylib's */
#define qoi_encode fr_qoi_encode
#define qoi_decode fr_qoi_decode
#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include "qoi.h"
#pragma GCC diagnostic pop

void fr_thumb_image_free(fr_thumb_image *im)
{
    free(im->rgba);
    *im = (fr_thumb_image){0};
}

int fr_thumb_supported(const char *name)
{
    static const char *const exts[] = {"png", "jpg", "jpeg", "jpe", "jfif", "bmp", "gif", "tga", "psd", "hdr", "qoi"};
    const char *e = fr_ext(name);
    for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
        if (!strcasecmp(e, exts[i])) return 1;
    return 0;
}

int fr_thumb_class(float px)
{
    if (px <= FR_THUMB_NORMAL) return FR_THUMB_NORMAL;
    if (px <= FR_THUMB_LARGE) return FR_THUMB_LARGE;
    return FR_THUMB_XLARGE;
}

static const char *class_dir(int px)
{
    return px <= FR_THUMB_NORMAL ? "normal" : px <= FR_THUMB_LARGE ? "large" : "x-large";
}

/* ---- MD5 (RFC 1321) ---- */

static void md5_block(uint32_t h[4], const unsigned char *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) f = (b & c) | (~b & d), g = i;
        else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
        else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
        else f = c ^ (b | ~d), g = (7 * i) % 16;
        int r = R[(i / 16) * 4 + i % 4];
        uint32_t t = a + f + K[i] + m[g];
        a = d, d = c, c = b;
        b += t << r | t >> (32 - r);
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

void fr_md5_hex(const void *data, size_t n, char out[33])
{
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const unsigned char *p = data;
    size_t done = 0;
    for (; n - done >= 64; done += 64) md5_block(h, p + done);
    unsigned char tail[128] = {0};
    size_t rest = n - done, len = rest < 56 ? 64 : 128;
    memcpy(tail, p + done, rest);
    tail[rest] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[len - 8 + i] = (unsigned char)(bits >> (8 * i));
    md5_block(h, tail);
    if (len == 128) md5_block(h, tail + 64);
    for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", (unsigned)(h[i / 4] >> (8 * (i % 4))) & 0xff);
}

/* ---- names ---- */

char *fr_thumb_uri(const char *path)
{
    static const char keep[] = "-._~!$&'()*+,;=:@/";
    size_t n = strlen(path);
    char *out = fr_realloc(NULL, 8 + n * 3), *o = out;
    o += sprintf(o, "file://");
    for (const unsigned char *s = (const unsigned char *)path; *s; s++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr(keep, *s)) *o++ = (char)*s;
        else o += sprintf(o, "%%%02X", *s);
    }
    *o = 0;
    return out;
}

char *fr_thumb_cache_root(void)
{
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/') return fr_format("%s/ferret/thumbs", xdg);
    return fr_format("%s/.cache/ferret/thumbs", home && *home ? home : "/tmp");
}

char *fr_thumb_cache_path(const char *root, const char *path, int px)
{
    char *uri = fr_thumb_uri(path), md5[33];
    fr_md5_hex(uri, strlen(uri), md5);
    free(uri);
    return fr_format("%s/%s/%s.png", root, class_dir(px), md5);
}

/* ---- pixels ---- */

void fr_thumb_fit(int w, int h, int max, int *ow, int *oh)
{
    if (w <= max && h <= max) {
        *ow = w, *oh = h;
        return;
    }
    if (w >= h) *ow = max, *oh = (int)(((int64_t)h * max + w / 2) / w);
    else *oh = max, *ow = (int)(((int64_t)w * max + h / 2) / h);
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}

void fr_thumb_downscale(const unsigned char *src, int sw, int sh, int channels, unsigned char *dst, int dw, int dh)
{
    int *xs = fr_realloc(NULL, sizeof *xs * (size_t)(dw + 1));
    uint64_t *acc = fr_realloc(NULL, sizeof *acc * 4 * (size_t)dw);
    for (int x = 0; x <= dw; x++) xs[x] = (int)((int64_t)x * sw / dw);
    for (int dy = 0; dy < dh; dy++) {
        int y0 = (int)((int64_t)dy * sh / dh), y1 = (int)((int64_t)(dy + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        memset(acc, 0, sizeof *acc * 4 * (size_t)dw);
        for (int y = y0; y < y1; y++) {
            const unsigned char *row = src + (size_t)y * sw * channels;
            for (int dx = 0; dx < dw; dx++) {
                uint64_t *a = acc + dx * 4;
                int x1 = xs[dx + 1] > xs[dx] ? xs[dx + 1] : xs[dx] + 1;
                for (int x = xs[dx]; x < x1; x++) {
                    const unsigned char *p = row + (size_t)x * channels;
                    unsigned al = channels == 4 ? p[3] : 255;
                    a[0] += (uint64_t)p[0] * al, a[1] += (uint64_t)p[1] * al, a[2] += (uint64_t)p[2] * al, a[3] += al;
                }
            }
        }
        for (int dx = 0; dx < dw; dx++) {
            const uint64_t *a = acc + dx * 4;
            int x1 = xs[dx + 1] > xs[dx] ? xs[dx + 1] : xs[dx] + 1;
            uint64_t count = (uint64_t)(y1 - y0) * (uint64_t)(x1 - xs[dx]);
            unsigned char *o = dst + ((size_t)dy * dw + dx) * 4;
            for (int c = 0; c < 3; c++) o[c] = a[3] ? (unsigned char)((a[c] + a[3] / 2) / a[3]) : 0;
            o[3] = (unsigned char)((a[3] + count / 2) / count);
        }
    }
    free(acc);
    free(xs);
}

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int fits(int w, int h) { return w > 0 && h > 0 && (uint64_t)w * (uint64_t)h <= FR_THUMB_MAX_PIXELS; }

int fr_thumb_exif_orientation(const unsigned char *p, size_t n)
{
    if (n < 4 || p[0] != 0xff || p[1] != 0xd8) return 1;
    for (size_t at = 2; at + 4 <= n && p[at] == 0xff;) {
        size_t len = (size_t)p[at + 2] << 8 | p[at + 3];
        if (p[at + 1] == 0xda || len < 2 || len > n - at - 2) break;
        if (p[at + 1] == 0xe1 && len >= 8 + 8 && !memcmp(p + at + 4, "Exif\0\0", 6)) {
            const unsigned char *t = p + at + 10;
            size_t tl = len - 8;
            int le = t[0] == 'I';
#define U16(q) (le ? (unsigned)(q)[0] | (unsigned)(q)[1] << 8 : (unsigned)(q)[0] << 8 | (unsigned)(q)[1])
#define U32(q) (le ? U16(q) | U16((q) + 2) << 16 : U16(q) << 16 | U16((q) + 2))
            uint32_t ifd = U32(t + 4);
            if (ifd > tl - 2) return 1;
            unsigned count = U16(t + ifd);
            for (unsigned i = 0; i < count && ifd + 2 + 12 * (i + 1) <= tl; i++) {
                const unsigned char *e = t + ifd + 2 + 12 * i;
                if (U16(e) == 0x0112) {
                    unsigned v = U16(e + 8);
                    return v >= 1 && v <= 8 ? (int)v : 1;
                }
            }
#undef U32
#undef U16
            return 1;
        }
        at += 2 + len;
    }
    return 1;
}

void fr_thumb_orient(fr_thumb_image *im, int orientation)
{
    if (orientation < 2 || orientation > 8) return;
    int w = im->w, h = im->h, swap = orientation >= 5;
    int ow = swap ? h : w, oh = swap ? w : h;
    unsigned char *out = fr_realloc(NULL, (size_t)w * h * 4);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int sx, sy;
            switch (orientation) {
            case 2: sx = w - 1 - x, sy = y; break;
            case 3: sx = w - 1 - x, sy = h - 1 - y; break;
            case 4: sx = x, sy = h - 1 - y; break;
            case 5: sx = y, sy = x; break;
            case 6: sx = y, sy = h - 1 - x; break;
            case 7: sx = w - 1 - y, sy = h - 1 - x; break;
            default: sx = w - 1 - y, sy = x; break;
            }
            memcpy(out + ((size_t)y * ow + x) * 4, im->rgba + ((size_t)sy * w + sx) * 4, 4);
        }
    free(im->rgba);
    im->rgba = out, im->w = ow, im->h = oh;
}

int fr_thumb_decode(const unsigned char *data, size_t n, int max_px, fr_thumb_image *out)
{
    *out = (fr_thumb_image){0};
    if (n > (size_t)INT32_MAX) return -1;
    unsigned char *pixels = NULL;
    int w, h, channels;
    if (n >= 14 && !memcmp(data, "qoif", 4)) {
        if (!fits((int)be32(data + 4), (int)be32(data + 8))) return -1;
        qoi_desc d;
        pixels = qoi_decode(data, (int)n, &d, 4);
        w = (int)d.width, h = (int)d.height, channels = 4;
    } else {
        int comp;
        if (!stbi_info_from_memory(data, (int)n, &w, &h, &comp) || !fits(w, h)) return -1;
        channels = comp == 2 || comp == 4 ? 4 : 3;
        pixels = stbi_load_from_memory(data, (int)n, &w, &h, &comp, channels);
    }
    if (!pixels) return -1;
    fr_thumb_fit(w, h, max_px, &out->w, &out->h);
    out->rgba = fr_realloc(NULL, (size_t)out->w * out->h * 4);
    fr_thumb_downscale(pixels, w, h, channels, out->rgba, out->w, out->h);
    free(pixels);
    fr_thumb_orient(out, fr_thumb_exif_orientation(data, n));
    return 0;
}

void fr_thumb_round(fr_thumb_image *im)
{
    int s = im->w < im->h ? im->w : im->h;
    float r = s * 0.06f;
    if (r < 1.5f) return;
    int ri = (int)r + 1;
    for (int y = 0; y < ri && y < im->h; y++)
        for (int x = 0; x < ri && x < im->w; x++) {
            int inside = 0;
            for (int i = 0; i < 16; i++) {
                float dx = r - (x + (i % 4 + 0.5f) / 4), dy = r - (y + (i / 4 + 0.5f) / 4);
                inside += dx <= 0 || dy <= 0 || dx * dx + dy * dy <= r * r;
            }
            if (inside == 16) continue;
            int px[4][2] = {{x, y}, {im->w - 1 - x, y}, {x, im->h - 1 - y}, {im->w - 1 - x, im->h - 1 - y}};
            for (int k = 0; k < 4; k++) {
                unsigned char *a = im->rgba + ((size_t)px[k][1] * im->w + px[k][0]) * 4 + 3;
                *a = (unsigned char)((*a * inside + 8) / 16);
            }
        }
}

/* ---- the PNG on disk ---- */

static uint32_t crc32_of(const unsigned char *p, size_t n, uint32_t crc)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

typedef struct {
    unsigned char *v;
    int n, cap;
} bytes;

static void put(bytes *b, const void *p, size_t n)
{
    FR_GROW(*b, b->n + (int)n);
    memcpy(b->v + b->n, p, n);
    b->n += (int)n;
}

static void put_be32(bytes *b, uint32_t v)
{
    unsigned char p[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
    put(b, p, 4);
}

static void put_text(bytes *b, const char *key, const char *value)
{
    size_t kl = strlen(key) + 1, vl = strlen(value);
    put_be32(b, (uint32_t)(kl + vl));
    int start = b->n;
    put(b, "tEXt", 4);
    put(b, key, kl);
    put(b, value, vl);
    put_be32(b, crc32_of(b->v + start, (size_t)(b->n - start), 0));
}

static void png_out(void *ctx, void *data, int size) { put(ctx, data, (size_t)size); }

int fr_thumb_save(const char *cache_path, const fr_thumb_image *im, const char *uri, int64_t mtime, uint64_t size)
{
    bytes png = {0}, out = {0};
    if (!stbi_write_png_to_func(png_out, &png, im->w, im->h, 4, im->rgba, im->w * 4) || png.n < 33) {
        free(png.v);
        return -1;
    }
    char num[32];
    put(&out, png.v, 33); /* the signature and IHDR, which must come first */
    put_text(&out, "Thumb::URI", uri);
    snprintf(num, sizeof num, "%lld", (long long)mtime);
    put_text(&out, "Thumb::MTime", num);
    snprintf(num, sizeof num, "%llu", (unsigned long long)size);
    put_text(&out, "Thumb::Size", num);
    put(&out, png.v + 33, (size_t)(png.n - 33));
    free(png.v);

    char *dir = fr_strdup(cache_path), *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    int rc = -1;
    if (!fr_mkdirs(dir)) {
        chmod(dir, 0700);
        char *tmp = fr_format("%s.XXXXXX", cache_path);
        int fd = mkstemp(tmp);
        if (fd >= 0) {
            ssize_t w = write(fd, out.v, (size_t)out.n);
            if (!close(fd) && w == out.n && !rename(tmp, cache_path)) rc = 0;
            else unlink(tmp);
        }
        free(tmp);
    }
    free(dir);
    free(out.v);
    return rc;
}

static unsigned char *read_file(const char *path, size_t max, size_t *n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    unsigned char *buf = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && (uint64_t)st.st_size <= max) {
        buf = fr_realloc(NULL, (size_t)st.st_size);
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got != (size_t)st.st_size) free(buf), buf = NULL;
        *n = got;
    }
    close(fd);
    return buf;
}

/* the tEXt chunks before the image data match */
static int png_matches(const unsigned char *p, size_t n, const char *uri, int64_t mtime, uint64_t size)
{
    char want_mtime[32], want_size[32];
    snprintf(want_mtime, sizeof want_mtime, "%lld", (long long)mtime);
    snprintf(want_size, sizeof want_size, "%llu", (unsigned long long)size);
    int ok_uri = 0, ok_mtime = 0, ok_size = 0;
    if (n < 8 || memcmp(p, "\x89PNG\r\n\x1a\n", 8)) return 0;
    for (size_t at = 8; at + 12 <= n;) {
        uint32_t len = be32(p + at);
        if (len > n - at - 12) return 0;
        const unsigned char *type = p + at + 4, *data = p + at + 8;
        if (!memcmp(type, "IDAT", 4) || !memcmp(type, "IEND", 4)) break;
        if (!memcmp(type, "tEXt", 4)) {
            const unsigned char *nul = memchr(data, 0, len);
            if (nul) {
                const char *key = (const char *)data, *value = (const char *)nul + 1;
                size_t vl = len - (size_t)(nul + 1 - data);
                if (!strcmp(key, "Thumb::URI")) ok_uri = vl == strlen(uri) && !memcmp(value, uri, vl);
                else if (!strcmp(key, "Thumb::MTime")) ok_mtime = vl == strlen(want_mtime) && !memcmp(value, want_mtime, vl);
                else if (!strcmp(key, "Thumb::Size")) ok_size = vl == strlen(want_size) && !memcmp(value, want_size, vl);
            }
        }
        at += 12 + len;
    }
    return ok_uri && ok_mtime && ok_size;
}

int fr_thumb_load(const char *cache_path, const char *uri, int64_t mtime, uint64_t size, fr_thumb_image *out)
{
    *out = (fr_thumb_image){0};
    size_t n;
    unsigned char *p = read_file(cache_path, 16u << 20, &n);
    if (!p) return -1;
    int rc = -1, w, h, comp;
    if (png_matches(p, n, uri, mtime, size)) {
        unsigned char *pixels = stbi_load_from_memory(p, (int)n, &w, &h, &comp, 4);
        if (pixels && w <= FR_THUMB_XLARGE && h <= FR_THUMB_XLARGE) {
            out->rgba = pixels, out->w = w, out->h = h;
            rc = 0;
        } else free(pixels);
    }
    free(p);
    return rc;
}

int fr_thumb_make(const char *root, const char *path, int64_t mtime, uint64_t size, int px, fr_thumb_image *out)
{
    char *uri = root ? fr_thumb_uri(path) : NULL, *cached = root ? fr_thumb_cache_path(root, path, px) : NULL;
    int rc = cached && !fr_thumb_load(cached, uri, mtime, size, out) ? 0 : -1;
    if (rc) {
        size_t n;
        unsigned char *data = read_file(path, FR_THUMB_MAX_BYTES, &n);
        rc = data ? fr_thumb_decode(data, n, px, out) : -1;
        free(data);
        if (!rc && cached) fr_thumb_save(cached, out, uri, mtime, size);
    }
    free(uri);
    free(cached);
    return rc;
}

/* ---- the worker ---- */

typedef struct {
    char *path;
    int64_t mtime;
    uint64_t size;
    int px;
} req;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static int started, stopping, working;
static char *cache_root;
static void (*wake_fn)(void);
static req current;
static struct {
    req *v;
    int n, cap;
} queue;
static struct {
    fr_thumb_done *v;
    int n, cap;
} done;

static int same(const char *path, int64_t mtime, uint64_t size, int px, const char *path2, int64_t mtime2, uint64_t size2, int px2)
{
    return px == px2 && mtime == mtime2 && size == size2 && !strcmp(path, path2);
}

static void *run(void *unused)
{
    pthread_mutex_lock(&lock);
    for (;;) {
        while (!stopping && !queue.n) pthread_cond_wait(&cond, &lock);
        if (stopping) break;
        current = queue.v[0];
        memmove(queue.v, queue.v + 1, sizeof *queue.v * (size_t)--queue.n);
        working = 1;
        pthread_mutex_unlock(&lock);

        fr_thumb_done d = {current.path, current.mtime, current.size, current.px, {0}};
        if (!fr_thumb_make(cache_root, d.path, d.mtime, d.size, d.px, &d.im)) fr_thumb_round(&d.im);

        pthread_mutex_lock(&lock);
        working = 0;
        current = (req){0};
        FR_PUSH(done, d);
        if (wake_fn) wake_fn();
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void fr_thumbs_start(const char *root, void (*wake)(void))
{
    if (started) return;
    cache_root = root ? fr_strdup(root) : NULL;
    wake_fn = wake;
    stopping = 0;
    if (pthread_create(&worker, NULL, run, NULL)) return;
    started = 1;
}

void fr_thumbs_stop(void)
{
    if (!started) return;
    pthread_mutex_lock(&lock);
    stopping = 1;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
    pthread_join(worker, NULL);
    started = 0;
    for (int i = 0; i < queue.n; i++) free(queue.v[i].path);
    queue.n = 0;
    for (int i = 0; i < done.n; i++) fr_thumb_done_free(&done.v[i]);
    done.n = 0;
    free(cache_root);
    cache_root = NULL;
}

void fr_thumbs_request(const fr_thumb_req *reqs, int n)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < queue.n; i++) free(queue.v[i].path);
    queue.n = 0;
    for (int i = 0; i < n; i++) {
        const fr_thumb_req *r = &reqs[i];
        int skip = working && same(r->path, r->mtime, r->size, r->px, current.path, current.mtime, current.size, current.px);
        for (int k = 0; k < done.n && !skip; k++)
            skip = same(r->path, r->mtime, r->size, r->px, done.v[k].path, done.v[k].mtime, done.v[k].size, done.v[k].px);
        for (int k = 0; k < queue.n && !skip; k++)
            skip = same(r->path, r->mtime, r->size, r->px, queue.v[k].path, queue.v[k].mtime, queue.v[k].size, queue.v[k].px);
        if (!skip) FR_PUSH(queue, ((req){fr_strdup(r->path), r->mtime, r->size, r->px}));
    }
    if (queue.n) pthread_cond_signal(&cond);
    pthread_mutex_unlock(&lock);
}

int fr_thumbs_take(fr_thumb_done *out)
{
    pthread_mutex_lock(&lock);
    int got = done.n > 0;
    if (got) {
        *out = done.v[0];
        memmove(done.v, done.v + 1, sizeof *done.v * (size_t)--done.n);
    }
    pthread_mutex_unlock(&lock);
    return got;
}

void fr_thumb_done_free(fr_thumb_done *d)
{
    free(d->path);
    fr_thumb_image_free(&d->im);
}

int fr_thumbs_busy(void)
{
    pthread_mutex_lock(&lock);
    int busy = queue.n || working || done.n;
    pthread_mutex_unlock(&lock);
    return busy;
}

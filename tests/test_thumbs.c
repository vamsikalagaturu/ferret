#define _GNU_SOURCE
#include "check.h"
#include "thumbs/thumbs.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char dir[64];

static char *at(const char *name) { return fr_format("%s/%s", dir, name); }

static void write_bytes(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(p, 1, n, f) == n);
    if (f) fclose(f);
}

static unsigned char *read_bytes(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    rewind(f);
    unsigned char *p = malloc(*n);
    CHECK(fread(p, 1, *n, f) == *n);
    fclose(f);
    return p;
}

/* w x h, red rising left to right, green top to bottom, opaque */
static fr_thumb_image gradient(int w, int h)
{
    fr_thumb_image im = {malloc((size_t)w * h * 4), w, h};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *p = im.rgba + ((size_t)y * w + x) * 4;
            p[0] = (unsigned char)(x * 255 / (w - 1)), p[1] = (unsigned char)(y * 255 / (h - 1)), p[2] = 40, p[3] = 255;
        }
    return im;
}

static const unsigned char *pixel(const fr_thumb_image *im, int x, int y) { return im->rgba + ((size_t)y * im->w + x) * 4; }

static int near(int a, int b) { return abs(a - b) <= 3; }

static void test_names(void)
{
    char md5[33];
    fr_md5_hex("", 0, md5);
    CHECK_STR(md5, "d41d8cd98f00b204e9800998ecf8427e");
    fr_md5_hex("abc", 3, md5);
    CHECK_STR(md5, "900150983cd24fb0d6963f7d28e17f72");
    const char *eighty = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
    fr_md5_hex(eighty, 80, md5);
    CHECK_STR(md5, "57edf4a22be3c955ac49da2e2107b67a");
    char a56[57], a64[65];
    memset(a56, 'a', 56), a56[56] = 0, memset(a64, 'a', 64), a64[64] = 0;
    fr_md5_hex(a56, 56, md5);
    CHECK_STR(md5, "3b0c8ac703f828b04c6c197006d17218");
    fr_md5_hex(a64, 64, md5);
    CHECK_STR(md5, "014842d480b571495a4a0363793f7367");

    char *uri = fr_thumb_uri("/home/a b/caf\xc3\xa9 #1 (x)%.png");
    CHECK_STR(uri, "file:///home/a%20b/caf%C3%A9%20%231%20(x)%25.png");
    free(uri);
    /* the specification's own example */
    char *p = fr_thumb_cache_path("/c", "/home/jens/photos/me.png", FR_THUMB_NORMAL);
    CHECK_STR(p, "/c/normal/c6ee772d9e49320e97ec29a7eb5b1697.png");
    free(p);
    p = fr_thumb_cache_path("/c", "/home/jens/photos/me.png", FR_THUMB_LARGE);
    CHECK_STR(p, "/c/large/c6ee772d9e49320e97ec29a7eb5b1697.png");
    free(p);
    p = fr_thumb_cache_path("/c", "/home/jens/photos/me.png", FR_THUMB_XLARGE);
    CHECK_STR(p, "/c/x-large/c6ee772d9e49320e97ec29a7eb5b1697.png");
    free(p);

    setenv("XDG_CACHE_HOME", "/x/cache", 1);
    p = fr_thumb_cache_root();
    CHECK_STR(p, "/x/cache/ferret/thumbs");
    free(p);
    setenv("XDG_CACHE_HOME", "relative", 1);
    setenv("HOME", "/h", 1);
    p = fr_thumb_cache_root();
    CHECK_STR(p, "/h/.cache/ferret/thumbs");
    free(p);

    CHECK(fr_thumb_supported("a.PNG") && fr_thumb_supported("b.jpeg") && fr_thumb_supported("c.qoi"));
    CHECK(fr_thumb_supported("d.hdr") && fr_thumb_supported("e.psd") && fr_thumb_supported("f.tga"));
    CHECK(!fr_thumb_supported("g.pdf") && !fr_thumb_supported("png") && !fr_thumb_supported("h.svg"));
    CHECK(fr_thumb_class(40) == 128 && fr_thumb_class(128) == 128 && fr_thumb_class(129) == 256);
    CHECK(fr_thumb_class(256) == 256 && fr_thumb_class(300) == 512 && fr_thumb_class(5000) == 512);
}

static void test_pixels(void)
{
    int w, h;
    fr_thumb_fit(1000, 500, 256, &w, &h);
    CHECK(w == 256 && h == 128);
    fr_thumb_fit(300, 1200, 256, &w, &h);
    CHECK(w == 64 && h == 256);
    fr_thumb_fit(10, 20, 256, &w, &h);
    CHECK(w == 10 && h == 20);
    fr_thumb_fit(5000, 1, 256, &w, &h);
    CHECK(w == 256 && h == 1);

    /* quadrants average to one pixel each */
    unsigned char q[4 * 4 * 3], out[2 * 2 * 4];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            unsigned char *p = q + (y * 4 + x) * 3;
            p[0] = x < 2 ? 200 : 0, p[1] = y < 2 ? 0 : 100, p[2] = (unsigned char)((x % 2) * 50);
        }
    fr_thumb_downscale(q, 4, 4, 3, out, 2, 2);
    CHECK(out[0] == 200 && out[1] == 0 && out[2] == 25 && out[3] == 255);
    CHECK(out[4] == 0 && out[5] == 0 && out[6] == 25);
    CHECK(out[8] == 200 && out[9] == 100);
    CHECK(out[12] == 0 && out[13] == 100 && out[15] == 255);

    /* a transparent neighbour lowers the alpha, not the colour */
    unsigned char two[8] = {255, 0, 0, 255, 0, 0, 0, 0}, one[4];
    fr_thumb_downscale(two, 2, 1, 4, one, 1, 1);
    CHECK(one[0] == 255 && one[1] == 0 && one[2] == 0 && one[3] == 128);

    fr_thumb_image sq = gradient(100, 100);
    fr_thumb_round(&sq);
    CHECK(pixel(&sq, 0, 0)[3] == 0 && pixel(&sq, 99, 0)[3] == 0 && pixel(&sq, 0, 99)[3] == 0 && pixel(&sq, 99, 99)[3] == 0);
    CHECK(pixel(&sq, 50, 50)[3] == 255 && pixel(&sq, 0, 50)[3] == 255 && pixel(&sq, 50, 0)[3] == 255);
    CHECK(pixel(&sq, 1, 1)[3] < 255);
    fr_thumb_image_free(&sq);
}

static void test_png(void)
{
    /* a PNG made here, decoded and scaled down */
    char *src = at("photo.png");
    fr_thumb_image im = gradient(600, 300);
    CHECK(fr_thumb_save(src, &im, "file:///anything", 1, 2) == 0);
    fr_thumb_image_free(&im);
    size_t n = 0;
    unsigned char *data = read_bytes(src, &n);
    CHECK(data != NULL);
    fr_thumb_image t;
    CHECK(fr_thumb_decode(data, n, 256, &t) == 0);
    CHECK(t.w == 256 && t.h == 128);
    if (t.rgba) {
        CHECK(near(pixel(&t, 0, 0)[0], 0) && near(pixel(&t, 0, 0)[1], 0) && pixel(&t, 0, 0)[2] == 40);
        CHECK(near(pixel(&t, 255, 127)[0], 255) && near(pixel(&t, 255, 127)[1], 255));
        CHECK(near(pixel(&t, 128, 64)[0], 128) && near(pixel(&t, 128, 64)[1], 128) && pixel(&t, 128, 64)[3] == 255);
    }
    fr_thumb_image_free(&t);

    /* an image claiming more pixels than allowed is not decoded: IHDR's width and height at 16 and 20 */
    unsigned char *big = malloc(n);
    memcpy(big, data, n);
    big[16] = 0, big[17] = 0, big[18] = 0x4e, big[19] = 0x20; /* 20000 */
    big[20] = 0, big[21] = 0, big[22] = 0x4e, big[23] = 0x20;
    CHECK(fr_thumb_decode(big, n, 256, &t) == -1 && !t.rgba);
    free(big);
    CHECK(fr_thumb_decode((const unsigned char *)"not an image", 12, 256, &t) == -1);
    free(data);

    /* the cached copy is valid for the same URI, mtime and size only */
    char *cached = at("c.png");
    im = gradient(300, 150);
    CHECK(fr_thumb_save(cached, &im, "file:///p/a.png", 1700000000, 12345) == 0);
    fr_thumb_image_free(&im);
    CHECK(fr_thumb_load(cached, "file:///p/a.png", 1700000000, 12345, &t) == 0 && t.w == 300 && t.h == 150);
    fr_thumb_image_free(&t);
    CHECK(fr_thumb_load(cached, "file:///p/a.png", 1700000001, 12345, &t) == -1);
    CHECK(fr_thumb_load(cached, "file:///p/a.png", 1700000000, 12346, &t) == -1);
    CHECK(fr_thumb_load(cached, "file:///p/b.png", 1700000000, 12345, &t) == -1);
    unlink(cached);
    free(cached);

    /* made once, then read from the disk cache; a changed mtime makes it again */
    char *root = at("cache");
    struct stat st;
    CHECK(stat(src, &st) == 0);
    CHECK(fr_thumb_make(root, src, st.st_mtime, (uint64_t)st.st_size, FR_THUMB_LARGE, &t) == 0 && t.w == 256 && t.h == 128);
    fr_thumb_image_free(&t);
    char *path = fr_thumb_cache_path(root, src, FR_THUMB_LARGE), *uri = fr_thumb_uri(src);
    struct stat cst;
    CHECK(stat(path, &cst) == 0 && (cst.st_mode & 0777) == 0600);
    CHECK(fr_thumb_load(path, uri, st.st_mtime, (uint64_t)st.st_size, &t) == 0 && t.w == 256);
    fr_thumb_image_free(&t);
    CHECK(fr_thumb_make(root, src, st.st_mtime + 5, (uint64_t)st.st_size, FR_THUMB_LARGE, &t) == 0);
    fr_thumb_image_free(&t);
    CHECK(fr_thumb_load(path, uri, st.st_mtime + 5, (uint64_t)st.st_size, &t) == 0);
    fr_thumb_image_free(&t);
    CHECK(fr_thumb_load(path, uri, st.st_mtime, (uint64_t)st.st_size, &t) == -1);
    unlink(path);
    char *large = at("cache/large");
    rmdir(large);
    rmdir(root);
    unlink(src);
    free(large), free(path), free(uri), free(root), free(src);
}

static void put_le32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)(v >> 16), p[3] = (unsigned char)(v >> 24); }

static void test_other_formats(void)
{
    /* a 24-bit BMP, 4 x 2, bottom-up: the top row blue, the bottom row white */
    unsigned char bmp[54 + 2 * 12] = {'B', 'M'};
    put_le32(bmp + 2, sizeof bmp), put_le32(bmp + 10, 54), put_le32(bmp + 14, 40);
    put_le32(bmp + 18, 4), put_le32(bmp + 22, 2);
    bmp[26] = 1, bmp[28] = 24;
    for (int i = 0; i < 4; i++) {
        memset(bmp + 54 + i * 3, 255, 3);
        bmp[54 + 12 + i * 3] = 255, bmp[54 + 12 + i * 3 + 1] = 0, bmp[54 + 12 + i * 3 + 2] = 0;
    }
    fr_thumb_image t;
    CHECK(fr_thumb_decode(bmp, sizeof bmp, 256, &t) == 0 && t.w == 4 && t.h == 2);
    if (t.rgba) {
        CHECK(pixel(&t, 0, 0)[0] == 0 && pixel(&t, 0, 0)[2] == 255 && pixel(&t, 0, 0)[3] == 255);
        CHECK(pixel(&t, 3, 1)[0] == 255 && pixel(&t, 3, 1)[1] == 255);
    }
    fr_thumb_image_free(&t);

    /* a QOI, 2 x 1, each pixel as QOI_OP_RGBA */
    unsigned char qoi[14 + 10 + 8] = {'q', 'o', 'i', 'f', 0, 0, 0, 2, 0, 0, 0, 1, 4, 0,
                                      0xff, 10, 20, 30, 255, 0xff, 200, 100, 50, 128, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK(fr_thumb_decode(qoi, sizeof qoi, 256, &t) == 0 && t.w == 2 && t.h == 1);
    if (t.rgba) CHECK(pixel(&t, 0, 0)[0] == 10 && pixel(&t, 1, 0)[0] == 200 && pixel(&t, 1, 0)[3] == 128);
    fr_thumb_image_free(&t);
}

static void test_orientation(void)
{
    /* SOI, APP1 "Exif" with a little-endian TIFF whose IFD0 holds Orientation (0x0112, SHORT) = 6, then SOS */
    unsigned char le[] = {0xff, 0xd8, 0xff, 0xe1, 0, 2 + 6 + 8 + 2 + 12 + 4, 'E', 'x', 'i', 'f', 0, 0,
                          'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0,
                          0xff, 0xda};
    CHECK(fr_thumb_exif_orientation(le, sizeof le) == 6);
    unsigned char be[] = {0xff, 0xd8, 0xff, 0xe0, 0, 4, 1, 2, /* an APP0 first */
                          0xff, 0xe1, 0, 2 + 6 + 8 + 2 + 12 + 4, 'E', 'x', 'i', 'f', 0, 0,
                          'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1, 0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 8, 0, 0, 0, 0, 0, 0};
    CHECK(fr_thumb_exif_orientation(be, sizeof be) == 8);
    be[11] = 20; /* the segment cut short: its IFD entry no longer fits */
    CHECK(fr_thumb_exif_orientation(be, sizeof be) == 1);
    CHECK(fr_thumb_exif_orientation((const unsigned char *)"\x89PNG", 4) == 1);

    /* A B over C D, turned and mirrored */
    static const unsigned char abcd[4] = {'A', 'B', 'C', 'D'};
    static const char *const want[9] = {"", "ABCD", "BADC", "DCBA", "CDAB", "ACBD", "CADB", "DBCA", "BDAC"};
    for (int o = 1; o <= 8; o++) {
        fr_thumb_image im = {malloc(16), 2, 2};
        for (int i = 0; i < 4; i++) memset(im.rgba + i * 4, abcd[i], 4);
        fr_thumb_orient(&im, o);
        char got[5] = {(char)im.rgba[0], (char)im.rgba[4], (char)im.rgba[8], (char)im.rgba[12], 0};
        CHECK_STR(got, want[o]);
        fr_thumb_image_free(&im);
    }
    fr_thumb_image wide = {calloc(3 * 1, 4), 3, 1};
    fr_thumb_orient(&wide, 6);
    CHECK(wide.w == 1 && wide.h == 3);
    fr_thumb_image_free(&wide);
}

static int wakes;
static void wake(void) { __atomic_add_fetch(&wakes, 1, __ATOMIC_RELAXED); }

static void test_worker(void)
{
    char *root = at("wcache"), *a = at("a.png"), *b = at("b.png"), *junk = at("junk.png");
    fr_thumb_image im = gradient(400, 400);
    fr_thumb_save(a, &im, "file:///a", 0, 0);
    fr_thumb_save(b, &im, "file:///b", 0, 0);
    fr_thumb_image_free(&im);
    write_bytes(junk, "junk", 4);
    struct stat sa, sb;
    CHECK(stat(a, &sa) == 0 && stat(b, &sb) == 0);

    fr_thumbs_start(root, wake);
    fr_thumb_req reqs[] = {{a, sa.st_mtime, (uint64_t)sa.st_size, FR_THUMB_NORMAL},
                           {b, sb.st_mtime, (uint64_t)sb.st_size, FR_THUMB_XLARGE},
                           {junk, 0, 4, FR_THUMB_NORMAL}};
    fr_thumbs_request(reqs, 3);
    int got = 0, ok_a = 0, ok_b = 0, none_junk = 0, taken[3] = {0};
    for (int tries = 0; got < 3 && tries < 500; tries++) {
        fr_thumb_done d;
        if (!fr_thumbs_take(&d)) {
            /* the window asks each frame for what it still lacks: in flight or done, nothing is made twice */
            fr_thumb_req again[3];
            int n = 0;
            for (int i = 0; i < 3; i++)
                if (!taken[i]) again[n++] = reqs[i];
            fr_thumbs_request(again, n);
            nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
            continue;
        }
        got++;
        for (int i = 0; i < 3; i++)
            if (!strcmp(d.path, reqs[i].path)) taken[i] = 1;
        if (!strcmp(d.path, a)) ok_a = d.im.w == 128 && d.im.h == 128 && pixel(&d.im, 0, 0)[3] == 0 && pixel(&d.im, 64, 64)[3] == 255;
        if (!strcmp(d.path, b)) ok_b = d.im.w == 400 && d.im.h == 400;
        if (!strcmp(d.path, junk)) none_junk = !d.im.rgba;
        fr_thumb_done_free(&d);
    }
    CHECK(got == 3 && ok_a && ok_b && none_junk);
    CHECK(wakes >= 3);
    nanosleep(&(struct timespec){0, 50 * 1000 * 1000}, NULL);
    fr_thumb_done d;
    CHECK(!fr_thumbs_take(&d)); /* re-asking did not make any twice */
    CHECK(!fr_thumbs_busy());

    /* a list without them drops what was not started */
    fr_thumbs_request(reqs, 0);
    CHECK(!fr_thumbs_busy());
    fr_thumbs_stop();

    char *p;
    p = fr_thumb_cache_path(root, a, FR_THUMB_NORMAL), unlink(p), free(p);
    p = fr_thumb_cache_path(root, b, FR_THUMB_XLARGE), unlink(p), free(p);
    p = at("wcache/normal"), rmdir(p), free(p);
    p = at("wcache/x-large"), rmdir(p), free(p);
    rmdir(root);
    unlink(a), unlink(b), unlink(junk);
    free(root), free(a), free(b), free(junk);
}

int main(void)
{
    snprintf(dir, sizeof dir, "/tmp/ferret-thumbs-XXXXXX");
    CHECK(mkdtemp(dir) != NULL);
    test_names();
    test_pixels();
    test_png();
    test_other_formats();
    test_orientation();
    test_worker();
    CHECK(rmdir(dir) == 0);
    return CHECK_DONE();
}

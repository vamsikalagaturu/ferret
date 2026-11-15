#define _GNU_SOURCE
#include "image.h"
#include "core/util.h"
#include "disks.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK (4u * 1024 * 1024)

int dk_image_check(const char *path, char *why, size_t len)
{
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, len, "Cannot read %s.", path);
        return -1;
    }
    unsigned char head[512];
    ssize_t n = pread(fd, head, sizeof head, 0);
    close(fd);
    if (n < 512 || head[510] != 0x55 || head[511] != 0xAA) {
        snprintf(why, len, "%s has no boot record, so it will not boot from a USB stick written this way. Linux ISOs have one; Windows ISOs do not.", name);
        return -1;
    }
    return 0;
}

static int write_all(int fd, const unsigned char *p, size_t n)
{
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k, n -= (size_t)k;
    }
    return 0;
}

static size_t read_full(int fd, unsigned char *p, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, p + got, n - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        got += (size_t)k;
    }
    return got;
}

static void report(void (*progress)(void *, double, const char *), void *ctx, const char *verb, uint64_t done, uint64_t size, double *at)
{
    if (!progress) return;
    double now = fr_now();
    if (done != size && now - *at < 0.2) return;
    *at = now;
    char a[32], b[32], text[96];
    dk_size_text(done, a, sizeof a), dk_size_text(size, b, sizeof b);
    snprintf(text, sizeof text, "%s %s of %s", verb, a, b);
    progress(ctx, size ? (double)done / (double)size : 1, text);
}

char *dk_image_write(int fd, const char *image, uint64_t size, void (*progress)(void *, double, const char *), void *ctx)
{
    char *error = NULL;
    unsigned char *buf = NULL;
    int src = open(image, O_RDONLY | O_CLOEXEC);
    if (src < 0) {
        error = fr_format("Cannot read %s.", image);
        goto out;
    }
    buf = fr_realloc(NULL, CHUNK);
    dk_sha256 written, back;
    dk_sha256_init(&written);
    double at = 0;
    for (uint64_t done = 0; done < size;) {
        size_t want = size - done < CHUNK ? (size_t)(size - done) : CHUNK;
        if (read_full(src, buf, want) != want) {
            error = fr_format("Reading %s failed.", image);
            goto out;
        }
        if (write_all(fd, buf, want)) {
            error = fr_format("Writing to the stick failed: %s.", strerror(errno));
            goto out;
        }
        dk_sha256_add(&written, buf, want);
        done += want;
        report(progress, ctx, "Writing", done, size, &at);
    }
    if (progress) progress(ctx, 1, "Flushing to the stick");
    if (fdatasync(fd)) {
        error = fr_format("Flushing failed: %s.", strerror(errno));
        goto out;
    }
    /* the read-back must come from the stick itself, not the page cache */
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    if (lseek(fd, 0, SEEK_SET) != 0) {
        error = fr_strdup("Cannot rewind the stick for verifying.");
        goto out;
    }
    dk_sha256_init(&back);
    at = 0;
    for (uint64_t done = 0; done < size;) {
        size_t want = size - done < CHUNK ? (size_t)(size - done) : CHUNK;
        if (read_full(fd, buf, want) != want) {
            error = fr_strdup("Reading the stick back failed.");
            goto out;
        }
        dk_sha256_add(&back, buf, want);
        done += want;
        report(progress, ctx, "Verifying", done, size, &at);
    }
    unsigned char a[32], b[32];
    dk_sha256_end(&written, a);
    dk_sha256_end(&back, b);
    if (memcmp(a, b, 32)) error = fr_strdup("Verification failed: the stick does not hold what was written. The stick may be faulty or fake.");
out:
    free(buf);
    if (src >= 0) close(src);
    close(fd);
    return error;
}

/* ---- SHA-256, FIPS 180-4 ---- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }

static void compress(dk_sha256 *s, const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ w[i - 15] >> 3;
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

void dk_sha256_init(dk_sha256 *s)
{
    static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, h0, sizeof h0);
    s->bytes = 0, s->used = 0;
}

void dk_sha256_add(dk_sha256 *s, const void *data, size_t n)
{
    const unsigned char *p = data;
    s->bytes += n;
    while (n) {
        size_t k = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->block + s->used, p, k);
        s->used += k, p += k, n -= k;
        if (s->used == 64) compress(s, s->block), s->used = 0;
    }
}

void dk_sha256_end(dk_sha256 *s, unsigned char out[32])
{
    uint64_t bits = s->bytes * 8;
    unsigned char pad = 0x80, zero = 0;
    dk_sha256_add(s, &pad, 1);
    while (s->used != 56) dk_sha256_add(s, &zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    dk_sha256_add(s, len, 8);
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) out[4 * i + k] = (unsigned char)(s->h[i] >> (24 - 8 * k));
}

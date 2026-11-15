#include "hash.h"
#include <string.h>

static uint32_t rotl(uint32_t x, int n) { return x << n | x >> (32 - n); }
static uint32_t rotr(uint32_t x, int n) { return x >> n | x << (32 - n); }

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3]; }

/* ---- MD5, RFC 1321 section 3.4 ---- */

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const int md5_s[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};

static void md5_block(uint32_t *h, const uint8_t *p)
{
    uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++) m[i] = le32(p + 4 * i);
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g, round = i / 16;
        switch (round) {
        case 0: f = (b & c) | (~b & d), g = i; break;
        case 1: f = (d & b) | (~d & c), g = (5 * i + 1) % 16; break;
        case 2: f = b ^ c ^ d, g = (3 * i + 5) % 16; break;
        default: f = c ^ (b | ~d), g = (7 * i) % 16; break;
        }
        f += a + md5_k[i] + m[g];
        a = d, d = c, c = b;
        b += rotl(f, md5_s[round][i % 4]);
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

/* ---- SHA-1, FIPS 180-4 section 6.1 ---- */

static void sha1_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 16; t++) w[t] = be32(p + 4 * t);
    for (int t = 16; t < 80; t++) w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    for (int t = 0; t < 80; t++) {
        uint32_t f, k;
        if (t < 20) f = (b & c) | (~b & d), k = 0x5a827999;
        else if (t < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
        else if (t < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
        else f = b ^ c ^ d, k = 0xca62c1d6;
        uint32_t tmp = rotl(a, 5) + f + e + k + w[t];
        e = d, d = c, c = rotl(b, 30), b = a, a = tmp;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

/* ---- SHA-256, FIPS 180-4 section 6.2 ---- */

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[64], v[8];
    for (int t = 0; t < 16; t++) w[t] = be32(p + 4 * t);
    for (int t = 16; t < 64; t++) {
        uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    memcpy(v, h, sizeof v);
    for (int t = 0; t < 64; t++) {
        uint32_t e = v[4], a = v[0];
        uint32_t t1 = v[7] + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & v[5]) ^ (~e & v[6])) + sha256_k[t] + w[t];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & v[1]) ^ (a & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, sizeof v[0] * 7);
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += v[i];
}

/* ---- the shared padding and buffering ---- */

static void block(fr_hash *h, const uint8_t *p)
{
    switch (h->kind) {
    case FR_MD5: md5_block(h->s.h, p); break;
    case FR_SHA1: sha1_block(h->s.h, p); break;
    case FR_SHA256: sha256_block(h->s.h, p); break;
    }
}

void fr_hash_init(fr_hash *h, fr_hash_kind kind)
{
    static const uint32_t md5_h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    static const uint32_t sha1_h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    static const uint32_t sha256_h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memset(h, 0, sizeof *h);
    h->kind = kind;
    if (kind == FR_MD5) memcpy(h->s.h, md5_h, sizeof md5_h);
    else if (kind == FR_SHA1) memcpy(h->s.h, sha1_h, sizeof sha1_h);
    else memcpy(h->s.h, sha256_h, sizeof sha256_h);
}

void fr_hash_update(fr_hash *h, const void *data, size_t n)
{
    const uint8_t *p = data;
    size_t have = (size_t)(h->s.len % 64);
    h->s.len += n;
    if (have) {
        size_t take = 64 - have < n ? 64 - have : n;
        memcpy(h->s.block + have, p, take);
        p += take, n -= take, have += take;
        if (have < 64) return;
        block(h, h->s.block);
    }
    for (; n >= 64; p += 64, n -= 64) block(h, p);
    memcpy(h->s.block, p, n);
}

size_t fr_hash_size(fr_hash_kind kind) { return kind == FR_MD5 ? 16 : kind == FR_SHA1 ? 20 : 32; }

void fr_hash_final_hex(fr_hash *h, char *out)
{
    uint64_t bits = h->s.len * 8;
    uint8_t pad[72] = {0x80}, len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(h->kind == FR_MD5 ? bits >> (8 * i) : bits >> (56 - 8 * i));
    size_t have = (size_t)(h->s.len % 64), npad = (have < 56 ? 56 : 120) - have;
    fr_hash_update(h, pad, npad);
    fr_hash_update(h, len, 8);
    static const char hex[] = "0123456789abcdef";
    size_t words = fr_hash_size(h->kind) / 4;
    for (size_t i = 0; i < words; i++)
        for (int b = 0; b < 4; b++) {
            uint8_t byte = (uint8_t)(h->kind == FR_MD5 ? h->s.h[i] >> (8 * b) : h->s.h[i] >> (24 - 8 * b));
            *out++ = hex[byte >> 4];
            *out++ = hex[byte & 15];
        }
    *out = 0;
}

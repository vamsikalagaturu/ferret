#include "sniff.h"
#include <stdint.h>

/* the length of the sequence at p (n bytes left): >0 valid, 0 invalid, -1 valid so far but cut off */
static int sequence(const uint8_t *p, size_t n)
{
    uint8_t c = p[0];
    int len;
    uint8_t lo = 0x80, hi = 0xbf; /* the second byte's range (RFC 3629 section 4) */
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) len = 2;
    else if (c >= 0xe0 && c <= 0xef) {
        len = 3;
        if (c == 0xe0) lo = 0xa0;
        if (c == 0xed) hi = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        len = 4;
        if (c == 0xf0) lo = 0x90;
        if (c == 0xf4) hi = 0x8f;
    } else return 0;
    for (int i = 1; i < len; i++) {
        if ((size_t)i >= n) return -1;
        uint8_t b = p[i];
        if (i == 1 ? b < lo || b > hi : b < 0x80 || b > 0xbf) return 0;
    }
    return len;
}

int fr_looks_text(const void *buf, size_t n, int complete)
{
    const uint8_t *p = buf;
    for (size_t i = 0; i < n;) {
        if (!p[i]) return 0;
        int len = sequence(p + i, n - i);
        if (len < 0) return !complete;
        if (!len) return 0;
        i += (size_t)len;
    }
    return 1;
}

size_t fr_utf8_whole(const char *buf, size_t n)
{
    size_t i = 0;
    while (i < n) {
        int len = sequence((const uint8_t *)buf + i, n - i);
        if (len < 0) return i;
        i += len ? (size_t)len : 1;
    }
    return n;
}

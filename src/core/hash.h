#ifndef FR_HASH_H
#define FR_HASH_H
/* MD5 (RFC 1321), SHA-1 and SHA-256 (FIPS 180-4), fed in pieces of any size. */
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len; /* bytes fed so far */
    uint8_t block[64];
} fr_hash_state;

typedef enum { FR_MD5, FR_SHA1, FR_SHA256 } fr_hash_kind;

typedef struct {
    fr_hash_kind kind;
    fr_hash_state s;
} fr_hash;

void fr_hash_init(fr_hash *h, fr_hash_kind kind);
void fr_hash_update(fr_hash *h, const void *data, size_t n);
/* the digest as lowercase hex into out (33, 41 or 65 bytes with the NUL) */
void fr_hash_final_hex(fr_hash *h, char *out);
size_t fr_hash_size(fr_hash_kind kind); /* digest bytes: 16, 20, 32 */
#endif

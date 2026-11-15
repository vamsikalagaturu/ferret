#include "check.h"
#include "core/hash.h"

static void digest(fr_hash_kind kind, const char *s, size_t n, size_t piece, char *out)
{
    fr_hash h;
    fr_hash_init(&h, kind);
    for (size_t i = 0; i < n; i += piece) fr_hash_update(&h, s + i, n - i < piece ? n - i : piece);
    fr_hash_final_hex(&h, out);
}

static void check_vector(const char *s, const char *md5, const char *sha1, const char *sha256)
{
    static const size_t pieces[] = {1, 3, 63, 64, 65, 1 << 20};
    char out[65];
    size_t n = strlen(s);
    for (size_t p = 0; p < sizeof pieces / sizeof *pieces; p++) {
        digest(FR_MD5, s, n, pieces[p], out);
        CHECK_STR(out, md5);
        digest(FR_SHA1, s, n, pieces[p], out);
        CHECK_STR(out, sha1);
        digest(FR_SHA256, s, n, pieces[p], out);
        CHECK_STR(out, sha256);
    }
}

int main(void)
{
    /* RFC 1321 appendix A.5, FIPS 180-4's examples (csrc.nist.gov), and the usual pangram */
    check_vector("", "d41d8cd98f00b204e9800998ecf8427e", "da39a3ee5e6b4b0d3255bfef95601890afd80709",
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_vector("abc", "900150983cd24fb0d6963f7d28e17f72", "a9993e364706816aba3e25717850c26c9cd0d89d",
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "8215ef0796a20bcaaae116d3876c664a",
                 "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    check_vector("The quick brown fox jumps over the lazy dog", "9e107d9d372bb6826bd81d3542a419d6",
                 "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12",
                 "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
    check_vector("12345678901234567890123456789012345678901234567890123456789012345678901234567890",
                 "57edf4a22be3c955ac49da2e2107b67a", "50abf5706a150990a08b2c5ea40fa0e585554732",
                 "f371bc4a311f2b009eef952dd83ca80e2b60026c8e935592d0f9c308453c813e");
    char *million = malloc(1000000);
    memset(million, 'a', 1000000);
    million[999999] = 0;
    char out[65];
    fr_hash h;
    fr_hash_init(&h, FR_SHA256);
    fr_hash_update(&h, million, 999999);
    fr_hash_update(&h, "a", 1);
    fr_hash_final_hex(&h, out);
    CHECK_STR(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    fr_hash_init(&h, FR_SHA1);
    fr_hash_update(&h, million, 999999);
    fr_hash_update(&h, "a", 1);
    fr_hash_final_hex(&h, out);
    CHECK_STR(out, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    fr_hash_init(&h, FR_MD5);
    fr_hash_update(&h, million, 999999);
    fr_hash_update(&h, "a", 1);
    fr_hash_final_hex(&h, out);
    CHECK_STR(out, "7707d6ae4e027c70eea2a935c2296f21");
    free(million);
    return CHECK_DONE();
}

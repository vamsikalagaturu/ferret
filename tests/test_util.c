#include "check.h"
#include "core/util.h"

int main(void)
{
    CHECK(fr_natcmp("file2", "file10") < 0);
    CHECK(fr_natcmp("File", "file") == 0);
    CHECK(fr_natcmp("a", "ab") < 0);
    CHECK(fr_natcmp("img007", "img7") == 0);
    CHECK(fr_natcmp("b", "A") > 0);

    CHECK_STR(fr_ext("report.tar.gz"), "gz");
    CHECK_STR(fr_ext(".bashrc"), "");
    CHECK_STR(fr_ext("noext"), "");
    CHECK_STR(fr_ext("trailing."), "");
    CHECK(fr_stem_len("photo.jpg", 0) == 5);
    CHECK(fr_stem_len("photo.jpg", 1) == 9);

    CHECK(fr_contains_nocase("Holiday Photos", "photo"));
    CHECK(!fr_contains_nocase("abc", "abd"));

    char buf[32];
    fr_format_size(999, buf, sizeof buf);
    CHECK_STR(buf, "999 B");
    fr_format_size(2100000, buf, sizeof buf);
    CHECK_STR(buf, "2.1 MB");
    fr_format_size(88000, buf, sizeof buf);
    CHECK_STR(buf, "88 KB");

    const char *s = "aé€😀";
    int len = (int)strlen(s), cp, i = 0;
    i = fr_utf8_decode(s, len, i, &cp);
    CHECK(cp == 'a');
    i = fr_utf8_decode(s, len, i, &cp);
    CHECK(cp == 0xE9);
    i = fr_utf8_decode(s, len, i, &cp);
    CHECK(cp == 0x20AC);
    i = fr_utf8_decode(s, len, i, &cp);
    CHECK(cp == 0x1F600 && i == len);
    CHECK(fr_utf8_prev(s, len) == len - 4);
    CHECK(fr_utf8_next(s, len, 1) == 3);
    char enc[4];
    CHECK(fr_utf8_encode(0x20AC, enc) == 3 && !memcmp(enc, "€", 3));
    return CHECK_DONE();
}

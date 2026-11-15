#include "check.h"
#include "core/sniff.h"

int main(void)
{
    CHECK(fr_looks_text("", 0, 1));
    CHECK(fr_looks_text("hello\nworld\t\r\n", 14, 1));
    CHECK(fr_looks_text("aé€😀", strlen("aé€😀"), 1));
    CHECK(!fr_looks_text("a\0b", 3, 1));
    CHECK(!fr_looks_text("\xff\xfe", 2, 1));       /* UTF-16 BOM */
    CHECK(!fr_looks_text("\xc0\xaf", 2, 1));       /* overlong "/" */
    CHECK(!fr_looks_text("\xed\xa0\x80", 3, 1));   /* a surrogate */
    CHECK(!fr_looks_text("\xf4\x90\x80\x80", 4, 1)); /* past U+10FFFF */
    CHECK(!fr_looks_text("caf\xe9", 4, 1));         /* Latin-1 */
    CHECK(!fr_looks_text("\x80", 1, 0));            /* a stray continuation */
    /* "€" cut by the end of what was read */
    CHECK(fr_looks_text("ab\xe2\x82", 4, 0));
    CHECK(!fr_looks_text("ab\xe2\x82", 4, 1));
    CHECK(fr_utf8_whole("ab\xe2\x82", 4) == 2);
    CHECK(fr_utf8_whole("ab\xe2\x82\xac", 5) == 5);
    CHECK(fr_utf8_whole("\xf0\x9f\x98", 3) == 0);
    unsigned char elf[] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0};
    CHECK(!fr_looks_text(elf, sizeof elf, 0));
    return CHECK_DONE();
}

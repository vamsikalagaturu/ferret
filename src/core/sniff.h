#ifndef FR_SNIFF_H
#define FR_SNIFF_H
#include <stddef.h>

/* 1 when the bytes read like text: no NUL and valid UTF-8 (RFC 3629). A sequence cut off by the end of buf
 * counts as valid unless complete says buf is the whole file. */
int fr_looks_text(const void *buf, size_t n, int complete);
/* bytes of buf that hold whole UTF-8 sequences: n less a sequence cut off at the end */
size_t fr_utf8_whole(const char *buf, size_t n);
#endif

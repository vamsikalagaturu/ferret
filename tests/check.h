#ifndef FR_CHECK_H
#define FR_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                                   \
            check_failures++;                                                                                          \
        }                                                                                                              \
    } while (0)
#define CHECK_STR(a, b)                                                                                                \
    do {                                                                                                               \
        const char *a_ = (a), *b_ = (b);                                                                               \
        if (!a_ || !b_ || strcmp(a_, b_)) {                                                                            \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", b_ ? b_ : "(null)");  \
            check_failures++;                                                                                          \
        }                                                                                                              \
    } while (0)
#define CHECK_DONE() (check_failures ? (fprintf(stderr, "%d failed\n", check_failures), 1) : 0)
#endif

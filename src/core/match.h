#ifndef FR_MATCH_H
#define FR_MATCH_H
#include <stddef.h>

/* the whole name against a glob: * any run, ? one character (a UTF-8 code point); ASCII case ignored */
int fr_glob_match(const char *pattern, const char *name);
/* a search term: a glob when it has * or ?, else fuzzy (fr_fuzzy_score); ASCII case ignored */
int fr_name_matches(const char *term, const char *name);
int fr_fuzzy_score(const char *term, const char *name); /* higher is closer; -1 no match */

/* "photo-{n:3}" with n = 7 -> "photo-007". {n} the number, {n:W} zero-padded to W digits, {n:W:S} counting from S
 * instead of 1 (n is 1 for the first name). Anything else is kept as written. */
char *fr_number_expand(const char *pattern, int n);
#endif

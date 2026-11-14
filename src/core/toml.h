#ifndef FR_TOML_H
#define FR_TOML_H
/* The part of TOML (toml.io/en/v1.0.0) a settings file needs: bare or quoted keys, basic and literal strings,
 * booleans, arrays of strings (across lines, trailing comma allowed), [table] headers, # comments. Anything
 * else is an error for that line, which is skipped. */
#include "util.h"

enum { TOML_STRING, TOML_BOOL, TOML_STRINGS, TOML_NUMBER };

typedef struct {
    char *table; /* "" before the first header */
    char *key;
    int type;
    char *str;
    int b;
    double num;
    fr_strs strs;
} fr_toml_value;

typedef struct {
    fr_toml_value *v;
    int n, cap;
    int error_line; /* the first line that did not parse, 0 none */
} fr_toml;

fr_toml fr_toml_parse(const char *text);
/* the last value of key in table ("" for the top), NULL when absent or of another type */
const fr_toml_value *fr_toml_get(const fr_toml *t, const char *table, const char *key, int type);
void fr_toml_free(fr_toml *t);

/* writing: appends to a growing NUL-terminated text */
typedef struct {
    char *v;
    int n, cap;
} fr_toml_out;
void fr_toml_comment(fr_toml_out *o, const char *text); /* "# text"; "" writes an empty line */
void fr_toml_table(fr_toml_out *o, const char *name);
void fr_toml_string(fr_toml_out *o, const char *key, const char *value);
void fr_toml_bool(fr_toml_out *o, const char *key, int value);
void fr_toml_number(fr_toml_out *o, const char *key, double value);
void fr_toml_strings(fr_toml_out *o, const char *key, const fr_strs *values); /* one per line */
#endif

#ifndef FR_RENAME_H
#define FR_RENAME_H
/* Renaming many items at once: the new names from a rule, then an order of renames that never overwrites. */
#include "jobs/history.h"
#include <regex.h>
#include <stddef.h>

typedef struct {
    const char *find;    /* "" changes the whole name before the extension */
    const char *replace; /* {n} numbering as fr_number_expand(); with regex, \0..\9 the groups */
    int regex;           /* find is a POSIX extended regular expression; every match is replaced */
    regex_t re;
    int compiled;
} fr_rename_rule;

/* 0, or -1 with why in err */
int fr_rename_rule_compile(fr_rename_rule *r, char *err, size_t errlen);
/* the n-th name's new name (n from 1); malloc'd */
char *fr_rename_rule_apply(const fr_rename_rule *r, const char *name, int is_dir, int n);
void fr_rename_rule_free(fr_rename_rule *r);

/* Renames from.v[i] to to.v[i] (URIs). Items keeping their name are left out; a cycle (a -> b, b -> a) goes
 * through a temporary name. -1 with why in err when a new name is not valid, two items would get the same name,
 * or a name is taken by something not being renamed. */
int fr_rename_plan(const fr_strs *from, const fr_strs *to, fr_steps *out, char *err, size_t errlen);
#endif

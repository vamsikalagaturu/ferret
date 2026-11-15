#ifndef FR_PERMS_H
#define FR_PERMS_H
/* A permission change over a selection, as chmod and chown take one: bits set and cleared on each item (so what
 * differs between them and is not touched stays as it was), optionally down the trees, files and folders apart. */
#include <stdint.h>

typedef enum { PERM_ALL, PERM_FILES, PERM_FOLDERS } fr_perm_only;

typedef struct {
    uint32_t set, clear; /* 07777 bits */
    int recursive;       /* everything inside folders too */
    fr_perm_only only;   /* which items the bits go to (the owner and group go to all) */
    /* chmod's X: the execute bits in set go to folders, and to files only where someone may execute them already */
    int exec_x;
    int64_t uid, gid; /* -1 keeps them */
} fr_perm_change;

/* the mode an item gets: old where c leaves it alone. top: one of the chosen items, not something inside one (a
 * folder inside also gets execute wherever it gets read, as fr_mode_edit() does) */
uint32_t fr_perm_mode(const fr_perm_change *c, uint32_t old, int is_dir, int top);
/* "755", "0644", "4755": 1 with *mode, and *special set when a fourth digit gives the setuid, setgid and sticky
 * bits; 0 when s is not 3 or 4 octal digits */
int fr_mode_parse_octal(const char *s, uint32_t *mode, int *special);
/* a user (group 0) or group (group 1) by name or number; 0, or -ENOENT */
int fr_owner_lookup(const char *name, int group, uint32_t *id);
/* the name of a user or group id, into out ("" when it has none) */
void fr_owner_name(uint32_t id, int group, char *out, int len);
#endif

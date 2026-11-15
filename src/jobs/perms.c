#include "perms.h"
#include "vfs/vfs.h"
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t fr_perm_mode(const fr_perm_change *c, uint32_t old, int is_dir, int top)
{
    if ((c->only == PERM_FILES && is_dir) || (c->only == PERM_FOLDERS && !is_dir)) return old & 07777;
    uint32_t set = c->set;
    /* as chmod(1) has it: X is execute for folders, and for files already executable by someone */
    if (c->exec_x && !is_dir && !(old & 0111)) set &= ~0111u;
    return fr_mode_edit(old, set, c->clear, is_dir && !top);
}

int fr_mode_parse_octal(const char *s, uint32_t *mode, int *special)
{
    int n = 0;
    uint32_t m = 0;
    for (; s[n]; n++) {
        if (s[n] < '0' || s[n] > '7') return 0;
        m = m << 3 | (uint32_t)(s[n] - '0');
    }
    if (n != 3 && n != 4) return 0;
    *mode = m;
    *special = n == 4;
    return 1;
}

static int number(const char *s, uint32_t *id)
{
    if (!*s) return 0;
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end || v > 0xfffffffful) return 0;
    *id = (uint32_t)v;
    return 1;
}

int fr_owner_lookup(const char *name, int group, uint32_t *id)
{
    if (!name || !*name) return -ENOENT;
    if (group) {
        struct group *g = getgrnam(name);
        if (g) return *id = (uint32_t)g->gr_gid, 0;
    } else {
        struct passwd *p = getpwnam(name);
        if (p) return *id = (uint32_t)p->pw_uid, 0;
    }
    return number(name, id) ? 0 : -ENOENT;
}

void fr_owner_name(uint32_t id, int group, char *out, int len)
{
    const char *name = NULL;
    if (group) {
        struct group *g = getgrgid((gid_t)id);
        name = g ? g->gr_name : NULL;
    } else {
        struct passwd *p = getpwuid((uid_t)id);
        name = p ? p->pw_name : NULL;
    }
    snprintf(out, (size_t)len, "%s", name ? name : "");
}

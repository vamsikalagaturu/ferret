#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>

void fr_mode_rwx(uint32_t mode, char out[10])
{
    static const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[i] = mode & (1u << (8 - i)) ? rwx[i] : '-';
    static const struct {
        uint32_t bit;
        int at;
        char set, unset;
    } special[] = {{04000, 2, 's', 'S'}, {02000, 5, 's', 'S'}, {01000, 8, 't', 'T'}};
    for (int i = 0; i < 3; i++)
        if (mode & special[i].bit) out[special[i].at] = out[special[i].at] == '-' ? special[i].unset : special[i].set;
    out[9] = 0;
}

void fr_mode_octal(uint32_t mode, char out[6]) { snprintf(out, 6, "%04o", (unsigned)(mode & 07777)); }

uint32_t fr_mode_edit(uint32_t old, uint32_t set, uint32_t clear, int folder_inside)
{
    uint32_t m = ((old & ~clear) | set) & 07777;
    if (folder_inside) m |= (m & 0444) >> 2;
    return m;
}

static void add(int64_t *field, int64_t n) { __atomic_add_fetch(field, n, __ATOMIC_RELAXED); }

static void measure(const char *uri, const fr_entry *e, fr_tree_count *c, const int *cancel)
{
    if (e->is_link || !e->is_dir) {
        add(&c->files, 1);
        if (!e->is_link) __atomic_add_fetch(&c->bytes, e->size, __ATOMIC_RELAXED);
        return;
    }
    add(&c->folders, 1);
    fr_entries kids = {0};
    if (fr_vfs_list(uri, &kids)) add(&c->unreadable, 1);
    for (int i = 0; i < kids.n && !__atomic_load_n(cancel, __ATOMIC_RELAXED); i++) {
        char *child = fr_uri_join(uri, kids.v[i].name);
        measure(child, &kids.v[i], c, cancel);
        free(child);
    }
    fr_entries_free(&kids);
}

void fr_vfs_measure(const char *uri, fr_tree_count *c, const int *cancel)
{
    fr_entry e = {0};
    if (fr_vfs_stat(uri, &e)) add(&c->unreadable, 1);
    else measure(uri, &e, c, cancel);
    fr_entry_free(&e);
}

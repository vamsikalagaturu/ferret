/* From UDisks2's objects to the disk map's rows, and which operating system each partition belongs to. */
#define _GNU_SOURCE
#include "disks.h"
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#define ESP "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"

static int eq(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static int empty(const char *s) { return !s || !*s; }
static char *dup_or_null(const char *s) { return s ? fr_strdup(s) : NULL; }

static int in_list(const char *s, const char *const *list)
{
    for (; s && *list; list++)
        if (!strcmp(s, *list)) return 1;
    return 0;
}

static int has_mount(const fr_strs *m, const char *const *list)
{
    for (int i = 0; i < m->n; i++)
        if (in_list(m->v[i], list)) return 1;
    return 0;
}

static const char *const system_mounts[] = {"/", "/boot", "/boot/efi", "/efi", "/home", "/usr", "/var", NULL};
static const char *const running_mounts[] = {"/", "/boot", "/boot/efi", "/efi", NULL};

/* GPT GUIDs from the UEFI spec (2.10, table 5.7) and the UAPI Discoverable Partitions Specification; MBR ids as
 * UDisks2 reports them ("0x83") */
static const char *const type_names[][2] = {
    {ESP, "EFI system"},
    {"21686148-6449-6e6f-744e-656564454649", "BIOS boot"},
    {"e3c9e316-0b5c-4db8-817d-f92df00215ae", "Microsoft reserved"},
    {"ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", "Microsoft basic data"},
    {"de94bba4-06d1-4d40-a16a-bfd50179d6ac", "Windows recovery"},
    {"0fc63daf-8483-4772-8e79-3d69d8477de4", "Linux filesystem"},
    {"4f68bce3-e8cd-4db1-96e7-fbcaf984b709", "Linux root (x86-64)"},
    {"933ac7e1-2eb4-4f13-b844-0e14e2aef915", "Linux home"},
    {"bc13c2ff-59e6-4262-a352-b275fd6f7172", "Linux extended boot"},
    {"0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "Linux swap"},
    {"e6d6d379-f507-44c2-a23c-238f2a3df928", "Linux LVM"},
    {"ca7d7ccb-63ed-4c53-861c-1742536059cc", "Linux LUKS"},
    {"48465300-0000-11aa-aa11-00306543ecac", "Apple HFS+"},
    {"7c3457ef-0000-11aa-aa11-00306543ecac", "Apple APFS"},
    {"0x07", "NTFS / exFAT"},
    {"0x0b", "FAT32"},
    {"0x0c", "FAT32 (LBA)"},
    {"0x0e", "FAT16 (LBA)"},
    {"0x05", "Extended"},
    {"0x0f", "Extended (LBA)"},
    {"0x27", "Windows recovery"},
    {"0x82", "Linux swap"},
    {"0x83", "Linux"},
    {"0x8e", "Linux LVM"},
    {"0xef", "EFI system"},
};

static const char *const windows_types[] = {"e3c9e316-0b5c-4db8-817d-f92df00215ae", "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7",
                                            "de94bba4-06d1-4d40-a16a-bfd50179d6ac", "0x07", "0x27", NULL};
static const char *const linux_types[] = {"0fc63daf-8483-4772-8e79-3d69d8477de4", "4f68bce3-e8cd-4db1-96e7-fbcaf984b709",
                                          "933ac7e1-2eb4-4f13-b844-0e14e2aef915", "bc13c2ff-59e6-4262-a352-b275fd6f7172",
                                          "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "e6d6d379-f507-44c2-a23c-238f2a3df928",
                                          "ca7d7ccb-63ed-4c53-861c-1742536059cc", "0x82", "0x83", "0x8e", NULL};
static const char *const linux_fs[] = {"ext2", "ext3", "ext4", "btrfs", "xfs", "f2fs", "swap", NULL};

const char *dk_type_name(const char *part_type)
{
    for (size_t i = 0; part_type && i < sizeof type_names / sizeof *type_names; i++)
        if (!strcmp(part_type, type_names[i][0])) return type_names[i][1];
    return part_type ? part_type : "";
}

const char *dk_seg_name(const dk_seg *s)
{
    if (!empty(s->label)) return s->label;
    if (!empty(s->part_name)) return s->part_name;
    if (!s->device) return "";
    return strncmp(s->device, "/dev/", 5) ? s->device : s->device + 5;
}

/* ---- objects ---- */

void dk_obj_free(dk_obj *o)
{
    char **strs[] = {&o->path, &o->vendor, &o->model, &o->serial, &o->sort_key, &o->bus, &o->device, &o->drive_path,
                     &o->backing, &o->id_type, &o->id_label, &o->id_uuid, &o->id_usage, &o->table_path, &o->part_type,
                     &o->part_uuid, &o->part_name, &o->table_type, &o->cleartext, &o->job_operation};
    for (size_t i = 0; i < sizeof strs / sizeof *strs; i++) free(*strs[i]);
    fr_strs_free(&o->mounts);
    *o = (dk_obj){0};
}

void dk_objs_free(dk_objs *o)
{
    for (int i = 0; i < o->n; i++) dk_obj_free(&o->v[i]);
    free(o->v);
    *o = (dk_objs){0};
}

void dk_tree_free(dk_tree *t)
{
    dk_objs_free(&t->objs);
    for (int i = 0; i < t->resize.n; i++) free(t->resize.v[i].fs);
    free(t->resize.v);
    fr_strs_free(&t->formats);
    free(t->version);
    *t = (dk_tree){0};
}

const dk_obj *dk_find(const dk_objs *o, const char *path)
{
    for (int i = 0; path && i < o->n; i++)
        if (!strcmp(o->v[i].path, path)) return &o->v[i];
    return NULL;
}

static int none(const char *path) { return !path || !*path || !strcmp(path, "/"); }

const dk_obj *dk_drive_of(const dk_objs *o, const dk_obj *b)
{
    const dk_obj *backing = none(b->backing) ? NULL : dk_find(o, b->backing);
    const dk_obj *d = dk_find(o, backing ? backing->drive_path : b->drive_path);
    return d && d->drive ? d : NULL;
}

char *dk_root_drive(const dk_objs *o)
{
    static const char *const root[] = {"/", NULL};
    for (int i = 0; i < o->n; i++) {
        if (!has_mount(&o->v[i].mounts, root)) continue;
        const dk_obj *b = &o->v[i];
        for (int hops = 0; b && hops < 4; hops++) {
            if (!none(b->drive_path)) return fr_strdup(b->drive_path);
            b = none(b->backing) ? NULL : dk_find(o, b->backing);
        }
    }
    return NULL;
}

/* ---- the system ---- */

static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t le32(const unsigned char *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }

/* UEFI 2.10, 3.1.3 EFI_LOAD_OPTION: attributes, the device path's length, the description in UTF-16, the device
 * path; its Hard Drive node (10.3.5.1) names the ESP by its GPT partition GUID (signature type 2) */
int dk_boot_parse(const unsigned char *d, size_t n, dk_boot *out)
{
    *out = (dk_boot){0};
    if (n < 10) return -1;
    out->active = le32(d + 4) & 1;
    size_t path_len = le16(d + 8), at = 10;
    struct {
        char *v;
        int n, cap;
    } name = {0};
    for (; at + 1 < n && le16(d + at); at += 2) {
        char enc[4];
        int k = fr_utf8_encode((int)le16(d + at), enc);
        for (int i = 0; i < k; i++) FR_PUSH(name, enc[i]);
    }
    FR_PUSH(name, 0);
    out->name = name.v;
    at += 2;
    size_t end = at + path_len < n ? at + path_len : n;
    while (at + 4 <= end) {
        unsigned type = d[at], sub = d[at + 1], len = le16(d + at + 2);
        if (len < 4 || type == 0x7f) break;
        if (type == 0x04 && sub == 0x01 && len >= 42 && at + 42 <= end && d[at + 41] == 2) {
            const unsigned char *g = d + at + 24;
            free(out->part_uuid);
            out->part_uuid = fr_format("%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned)le32(g), le16(g + 4),
                                       le16(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
        }
        at += len;
    }
    if (out->part_uuid) return 0;
    free(out->name);
    *out = (dk_boot){0};
    return -1;
}

void dk_boot_read(const char *efivars_dir, dk_env *env)
{
    DIR *dir = opendir(efivars_dir);
    if (!dir) return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        const char *s = e->d_name;
        if (strncmp(s, "Boot", 4) || strcmp(s + 8, "-8be4df61-93ca-11d2-aa0d-00e098032b8c")) continue;
        int hex = 1;
        for (int i = 4; i < 8; i++) hex &= isxdigit((unsigned char)s[i]) && !islower((unsigned char)s[i]);
        if (!hex) continue;
        char *path = fr_format("%s/%s", efivars_dir, s);
        FILE *f = fopen(path, "rb");
        free(path);
        if (!f) continue;
        unsigned char buf[4096];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        dk_boot b;
        if (!dk_boot_parse(buf, n, &b)) FR_PUSH(env->boot, b);
    }
    closedir(dir);
}

void dk_swaps_read(const char *proc_swaps, fr_strs *out)
{
    FILE *f = fopen(proc_swaps, "r");
    if (!f) return;
    char line[1024];
    int first = 1;
    while (fgets(line, sizeof line, f)) {
        if (first) {
            first = 0;
            continue;
        }
        char dev[1024];
        if (sscanf(line, "%1023s", dev) == 1) FR_PUSH(*out, fr_strdup(dev));
    }
    fclose(f);
}

char *dk_os_release(const char *root)
{
    static const char *const rel[] = {"/etc/os-release", "/usr/lib/os-release"};
    for (int i = 0; i < 2; i++) {
        char *path = fr_format("%s%s", strcmp(root, "/") ? root : "", rel[i]);
        FILE *f = fopen(path, "r");
        free(path);
        if (!f) continue;
        char line[512], *name = NULL;
        while (!name && fgets(line, sizeof line, f)) {
            if (strncmp(line, "PRETTY_NAME=", 12)) continue;
            char *o = line, *s = line + 12;
            for (; *s && *s != '\n'; s++)
                if (*s != '"') *o++ = *s;
            *o = 0;
            name = fr_strdup(line);
        }
        fclose(f);
        if (name) return name;
    }
    return NULL;
}

void dk_env_read(dk_env *env)
{
    dk_boot_read("/sys/firmware/efi/efivars", env);
    env->running = dk_os_release("/");
    dk_swaps_read("/proc/swaps", &env->swaps);
}

void dk_env_free(dk_env *env)
{
    for (int i = 0; i < env->boot.n; i++) free(env->boot.v[i].name), free(env->boot.v[i].part_uuid);
    free(env->boot.v);
    free(env->running);
    fr_strs_free(&env->swaps);
    *env = (dk_env){0};
}

/* ---- segments and disks ---- */

void dk_seg_free(dk_seg *s)
{
    char **strs[] = {&s->key, &s->path, &s->device, &s->fs_type, &s->label, &s->uuid, &s->part_uuid, &s->part_name,
                     &s->part_type, &s->group, &s->was};
    for (size_t i = 0; i < sizeof strs / sizeof *strs; i++) free(*strs[i]);
    fr_strs_free(&s->mounts);
    *s = (dk_seg){0};
}

dk_seg dk_seg_copy(const dk_seg *s)
{
    dk_seg c = *s;
    char **strs[] = {&c.key, &c.path, &c.device, &c.fs_type, &c.label, &c.uuid, &c.part_uuid, &c.part_name,
                     &c.part_type, &c.group, &c.was};
    for (size_t i = 0; i < sizeof strs / sizeof *strs; i++) *strs[i] = dup_or_null(*strs[i]);
    c.mounts = fr_strs_copy(&s->mounts);
    return c;
}

dk_seg dk_free_seg(uint64_t offset, uint64_t size)
{
    return (dk_seg){.key = fr_format("free:%llu", (unsigned long long)offset), .kind = SEG_FREE, .offset = offset,
                    .size = size, .group_kind = GROUP_UNKNOWN};
}

void dk_disk_free(dk_disk *d)
{
    free(d->path), free(d->block), free(d->name), free(d->device), free(d->serial), free(d->table_type);
    free(d->connection);
    for (int i = 0; i < d->segs.n; i++) dk_seg_free(&d->segs.v[i]);
    free(d->segs.v);
    *d = (dk_disk){0};
}

dk_disk dk_disk_copy(const dk_disk *d)
{
    dk_disk c = *d;
    c.path = dup_or_null(d->path), c.block = dup_or_null(d->block), c.name = dup_or_null(d->name);
    c.device = dup_or_null(d->device), c.serial = dup_or_null(d->serial), c.table_type = dup_or_null(d->table_type);
    c.connection = dup_or_null(d->connection);
    c.segs = (dk_segs){0};
    for (int i = 0; i < d->segs.n; i++) FR_PUSH(c.segs, dk_seg_copy(&d->segs.v[i]));
    return c;
}

void dk_disks_free(dk_disks *d)
{
    for (int i = 0; i < d->n; i++) dk_disk_free(&d->v[i]);
    free(d->v);
    *d = (dk_disks){0};
}

dk_disks dk_disks_copy(const dk_disks *d)
{
    dk_disks c = {0};
    for (int i = 0; i < d->n; i++) FR_PUSH(c, dk_disk_copy(&d->v[i]));
    return c;
}

int dk_disk_index(const dk_disks *d, const char *path)
{
    for (int i = 0; path && i < d->n; i++)
        if (eq(d->v[i].path, path)) return i;
    return -1;
}

int dk_seg_index(const dk_segs *s, const char *key)
{
    for (int i = 0; key && i < s->n; i++)
        if (eq(s->v[i].key, key)) return i;
    return -1;
}

static uint64_t resize_modes(const dk_tree *t, const char *fs)
{
    if (empty(fs)) return DK_OFFLINE_SHRINK | DK_OFFLINE_GROW;
    for (int i = 0; i < t->resize.n; i++)
        if (eq(t->resize.v[i].fs, fs)) return t->resize.v[i].modes;
    return 0;
}

static dk_seg block_seg(const dk_tree *t, const dk_obj *b, uint64_t offset)
{
    dk_seg s = {
        .key = fr_strdup(b->path),
        .kind = b->partition ? SEG_PARTITION : SEG_FILESYSTEM,
        .path = fr_strdup(b->path),
        .device = fr_strdup(b->device ? b->device : ""),
        .number = b->number,
        .offset = offset,
        .size = b->size,
        .fs_type = fr_strdup(b->id_type ? b->id_type : ""),
        .label = fr_strdup(b->id_label ? b->id_label : ""),
        .uuid = fr_strdup(b->id_uuid ? b->id_uuid : ""),
        .part_uuid = fr_strdup(b->part_uuid ? b->part_uuid : ""),
        .part_name = fr_strdup(b->part_name ? b->part_name : ""),
        .part_type = fr_strdup(b->part_type ? b->part_type : ""),
        .has_fs = b->fs,
        .encrypted = b->encrypted || eq(b->id_type, "BitLocker"),
        .mounts = fr_strs_copy(&b->mounts),
        .system_mount = has_mount(&b->mounts, system_mounts),
        .swap_active = b->swap_active,
    };
    for (char *c = s.part_uuid; *c; c++) *c = (char)tolower((unsigned char)*c);
    s.type_name = dk_type_name(s.part_type);
    s.resize_modes = resize_modes(t, s.fs_type);
    struct statvfs v;
    if (s.mounts.n && !statvfs(s.mounts.v[0], &v) && v.f_blocks) {
        s.has_used = 1;
        s.used_total = (uint64_t)v.f_blocks * v.f_frsize;
        s.used = (uint64_t)(v.f_blocks - v.f_bfree) * v.f_frsize;
    }
    return s;
}

/* Which OS an ESP boots: active entries first; generic fallback loaders ("UEFI OS") only when nothing else points
 * at it, and then it stays unnamed. */
static const char *esp_owner(const dk_env *env, const char *part_uuid)
{
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < env->boot.n; i++) {
            const dk_boot *e = &env->boot.v[i];
            if (!eq(e->part_uuid, part_uuid) || (!pass && !e->active) || eq(e->name, "UEFI OS")) continue;
            return eq(e->name, "Windows Boot Manager") ? "Windows" : e->name;
        }
    return NULL;
}

static int linuxish(const dk_seg *s) { return in_list(s->part_type, linux_types) || in_list(s->fs_type, linux_fs); }

/* Direct evidence first: the running system's mounts and active swap, the firmware boot entry pointing at an ESP,
 * os-release of a mounted root, Microsoft partition types. Then unclaimed Linux partitions take the OS of an
 * adjacent Linux partition, marked inferred. */
static void assign_groups(dk_segs *segs, const dk_env *env)
{
    for (int i = 0; i < segs->n; i++) {
        dk_seg *s = &segs->v[i];
        if (s->kind == SEG_FREE) continue;
        char *group = NULL;
        int swap = 0;
        for (int k = 0; k < env->swaps.n; k++) swap |= eq(env->swaps.v[k], s->device);
        if ((has_mount(&s->mounts, running_mounts) || swap) && !empty(env->running)) group = fr_strdup(env->running);
        if (!group && (eq(s->part_type, ESP) || eq(s->part_type, "0xef"))) group = dup_or_null(esp_owner(env, s->part_uuid));
        for (int k = 0; !group && k < s->mounts.n; k++) group = dk_os_release(s->mounts.v[k]);
        if (!group && (in_list(s->part_type, windows_types) || eq(s->fs_type, "ntfs") || eq(s->fs_type, "BitLocker")))
            group = fr_strdup("Windows");
        s->group = group;
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < segs->n; i++) {
            dk_seg *s = &segs->v[i];
            if (s->kind == SEG_FREE || s->group || !linuxish(s)) continue;
            for (int j = i - 1; j <= i + 1; j += 2) {
                if (j < 0 || j >= segs->n) continue;
                const dk_seg *n = &segs->v[j];
                if (n->kind != SEG_FREE && n->group && !eq(n->group, "Windows")) {
                    s->group = fr_strdup(n->group);
                    s->inferred = 1;
                    break;
                }
            }
        }
    for (int i = 0; i < segs->n; i++) {
        dk_seg *s = &segs->v[i];
        if (s->kind == SEG_FREE) continue;
        s->group_kind = !s->group                ? GROUP_UNKNOWN
                        : eq(s->group, env->running) ? GROUP_RUNNING
                        : eq(s->group, "Windows")    ? GROUP_WINDOWS
                                                     : GROUP_OS;
        if (!s->group) s->group = fr_strdup("Unidentified");
    }
}

static int by_offset(const void *a, const void *b)
{
    const dk_obj *x = *(const dk_obj *const *)a, *y = *(const dk_obj *const *)b;
    return x->part_offset < y->part_offset ? -1 : x->part_offset > y->part_offset;
}

typedef struct {
    const char *key;
    dk_disk d;
} keyed_disk;

static int by_sort_key(const void *a, const void *b)
{
    const keyed_disk *x = a, *y = b;
    return strcmp(x->key, y->key);
}

void dk_build(const dk_tree *t, const dk_env *env, dk_disks *out)
{
    const dk_objs *o = &t->objs;
    char *root = dk_root_drive(o);
    struct {
        keyed_disk *v;
        int n, cap;
    } rows = {0};
    for (int di = 0; di < o->n; di++) {
        const dk_obj *drive = &o->v[di];
        if (!drive->drive) continue;
        const dk_obj *whole = NULL;
        struct {
            const dk_obj **v;
            int n, cap;
        } parts = {0};
        for (int i = 0; i < o->n; i++) {
            const dk_obj *b = &o->v[i];
            if (!b->block || !eq(b->drive_path, drive->path)) continue;
            if (b->partition) FR_PUSH(parts, b);
            else if (!whole) whole = b;
        }
        if (!whole) {
            free(parts.v);
            continue;
        }
        dk_disk d = {0};
        if (whole->table) {
            int k = 0;
            for (int i = 0; i < parts.n; i++)
                if (eq(parts.v[i]->table_path, whole->path) && !parts.v[i]->container) parts.v[k++] = parts.v[i];
            parts.n = k;
            if (parts.n) qsort(parts.v, (size_t)parts.n, sizeof *parts.v, by_offset);
            uint64_t cursor = DK_MIB;
            for (int i = 0; i < parts.n; i++) {
                uint64_t offset = parts.v[i]->part_offset;
                if (offset > cursor && offset - cursor >= DK_MIN_FREE) FR_PUSH(d.segs, dk_free_seg(cursor, offset - cursor));
                FR_PUSH(d.segs, block_seg(t, parts.v[i], offset));
                uint64_t end = offset + parts.v[i]->part_size;
                if (end > cursor) cursor = end;
            }
            if (whole->size > cursor && whole->size - cursor >= DK_MIN_FREE) FR_PUSH(d.segs, dk_free_seg(cursor, whole->size - cursor));
        } else if (!empty(whole->id_usage)) {
            FR_PUSH(d.segs, block_seg(t, whole, 0));
        } else if (whole->size) {
            FR_PUSH(d.segs, dk_free_seg(0, whole->size));
        }
        free(parts.v);
        assign_groups(&d.segs, env);
        char *name = fr_format("%s %s", drive->vendor ? drive->vendor : "", drive->model ? drive->model : "");
        char *s = name, *e = name + strlen(name);
        while (*s == ' ') s++;
        while (e > s && e[-1] == ' ') *--e = 0;
        d.path = fr_strdup(drive->path);
        d.block = fr_strdup(whole->path);
        d.device = fr_strdup(whole->device ? whole->device : "");
        d.name = fr_strdup(*s ? s : d.device);
        free(name);
        d.serial = fr_strdup(drive->serial ? drive->serial : "");
        d.table_type = fr_strdup(whole->table && whole->table_type ? whole->table_type : "");
        d.connection = fr_strdup(drive->bus ? drive->bus : "");
        d.size = whole->size;
        d.removable = drive->removable, d.ejectable = drive->ejectable, d.can_power_off = drive->can_power_off;
        d.root_disk = eq(drive->path, root);
        FR_PUSH(rows, ((keyed_disk){drive->sort_key ? drive->sort_key : "", d}));
    }
    if (rows.n) qsort(rows.v, (size_t)rows.n, sizeof *rows.v, by_sort_key);
    for (int i = 0; i < rows.n; i++) FR_PUSH(*out, rows.v[i].d);
    free(rows.v);
    free(root);
}

/* ---- sizes ---- */

void dk_size_text(uint64_t bytes, char *buf, size_t len)
{
    static const char *const units[] = {"KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    if (bytes < 1024) {
        snprintf(buf, len, "%llu B", (unsigned long long)bytes);
        return;
    }
    double v = (double)bytes / 1024;
    int u = 0;
    while (v >= 1024 && u < 5) v /= 1024, u++;
    snprintf(buf, len, "%.1f %s", v, units[u]);
}

int64_t dk_parse_size(const char *text)
{
    const char *s = text;
    while (*s == ' ' || *s == '\t') s++;
    if (!isdigit((unsigned char)*s)) return -1;
    char *end;
    double v = strtod(s, &end);
    if (end == s) return -1;
    for (const char *c = s; c < end; c++)
        if (!isdigit((unsigned char)*c) && *c != '.') return -1;
    s = end;
    while (*s == ' ' || *s == '\t') s++;
    int power = 0;
    const char *units = "KMGT", *u = *s ? strchr(units, toupper((unsigned char)*s)) : NULL;
    if (u) power = (int)(u - units) + 1, s++;
    if ((*s == 'i' || *s == 'I') && (s[1] == 'b' || s[1] == 'B')) s += 2;
    else if (*s == 'b' || *s == 'B') s++;
    while (*s == ' ' || *s == '\t') s++;
    if (*s) return -1;
    return (int64_t)floor(v * pow(1024.0, power));
}

/* ---- the backstop ---- */

const char *dk_refusal(const dk_objs *o, const char *root_drive, const char *object, const char *method)
{
    const dk_obj *b = dk_find(o, object);
    if (!b) return NULL;
    const char *drive = b->drive ? b->path : b->drive_path;
    static const char *const raw[] = {"Eject", "PowerOff", "OpenDevice", NULL};
    static const char *const destructive[] = {"Delete", "Format", "OpenDevice", "SetLabel", NULL};
    if (!none(drive) && eq(drive, root_drive) && in_list(method, raw)) return "Refused: the running system lives on this disk.";
    if ((b->mounts.n || b->swap_active) && in_list(method, destructive)) return "Refused: it is in use.";
    if (eq(method, "Unmount") && has_mount(&b->mounts, running_mounts)) return "Refused: the running system needs it.";
    return NULL;
}

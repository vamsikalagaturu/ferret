/* The disk manager's model, plan and scans on synthetic UDisks2 trees shaped like a real dual-boot machine:
 * nvme0n1 Windows (BitLocker) + an unmounted Pop!_OS, nvme1n1 the running Ubuntu, a USB stick, a USB disk with a
 * mounted Fedora. Nothing here talks to UDisks2 or writes to a device. */
#define _GNU_SOURCE
#include "check.h"
#include "disks/disks.h"
#include "disks/image.h"
#include "disks/plan.h"
#include "disks/usage.h"
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

#define GIB (1024ull * 1024 * 1024)
#define ESP "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
#define MSR "e3c9e316-0b5c-4db8-817d-f92df00215ae"
#define BASIC "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"
#define LINUX "0fc63daf-8483-4772-8e79-3d69d8477de4"
#define SWAP "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"
#define DRIVES "/org/freedesktop/UDisks2/drives/"
#define BLOCKS "/org/freedesktop/UDisks2/block_devices/"

static char tmp[256];

static dk_obj *drive(dk_tree *t, const char *name, const char *sort_key, const char *bus, int removable)
{
    FR_PUSH(t->objs, ((dk_obj){.path = fr_format(DRIVES "%s", name), .drive = 1, .vendor = fr_strdup(""), .model = fr_strdup(name),
                                .serial = fr_format("S-%s", name), .sort_key = fr_strdup(sort_key), .bus = fr_strdup(bus),
                                .removable = removable, .ejectable = removable, .can_power_off = removable}));
    return &t->objs.v[t->objs.n - 1];
}

static dk_obj *whole(dk_tree *t, const char *dev, const char *drive_name, uint64_t size, const char *table)
{
    FR_PUSH(t->objs, ((dk_obj){.path = fr_format(BLOCKS "%s", dev), .block = 1, .device = fr_format("/dev/%s", dev),
                                .drive_path = fr_format(DRIVES "%s", drive_name), .size = size, .table = table != NULL,
                                .table_type = table ? fr_strdup(table) : NULL, .id_usage = fr_strdup("")}));
    return &t->objs.v[t->objs.n - 1];
}

static dk_obj *part(dk_tree *t, const char *dev, const char *disk, const char *drive_name, unsigned n, uint64_t start_sector,
                    uint64_t size, const char *type, const char *uuid, const char *fs, const char *label, const char *mount)
{
    char *p = fr_format("%sp%u", dev, n);
    if (!strncmp(dev, "sd", 2)) free(p), p = fr_format("%s%u", dev, n);
    dk_obj o = {.path = fr_format(BLOCKS "%s", p), .block = 1, .partition = 1, .device = fr_format("/dev/%s", p),
                .drive_path = fr_format(DRIVES "%s", drive_name), .size = size, .table_path = fr_format(BLOCKS "%s", disk),
                .part_offset = start_sector * 512, .part_size = size, .number = n, .part_type = fr_strdup(type),
                .part_uuid = fr_strdup(uuid), .id_type = fr_strdup(fs ? fs : ""), .id_label = fr_strdup(label ? label : ""),
                .id_usage = fr_strdup(fs ? "filesystem" : ""), .fs = fs && strcmp(fs, "swap") && strcmp(fs, "BitLocker"),
                .swap = fs && !strcmp(fs, "swap")};
    if (mount) FR_PUSH(o.mounts, fr_strdup(mount));
    free(p);
    FR_PUSH(t->objs, o);
    return &t->objs.v[t->objs.n - 1];
}

static void machine(dk_tree *t, const char *fedora_root)
{
    drive(t, "Corsair_MP700", "00coldplug/00fixed/nvme0n1", "", 0);
    whole(t, "nvme0n1", "Corsair_MP700", 2000398934016ull, "gpt");
    part(t, "nvme0n1", "nvme0n1", "Corsair_MP700", 1, 2048, 272629760, ESP, "35c657cf-626b-43e0-bd89-ce998645a441", "vfat", "SYSTEM", NULL);
    part(t, "nvme0n1", "nvme0n1", "Corsair_MP700", 2, 534528, 16777216, MSR, "914b2141-1b6e-48a2-934e-c52f643b6953", NULL, NULL, NULL);
    part(t, "nvme0n1", "nvme0n1", "Corsair_MP700", 3, 567296, 829930078208ull, BASIC, "cb9325a7-d766-4d28-9545-66f51d56eeeb", "BitLocker", NULL, NULL)->encrypted = 1;
    part(t, "nvme0n1", "nvme0n1", "Corsair_MP700", 4, 3905325056ull, 872415232, BASIC, "7c478d0f-103f-4b13-8421-60fbc8058ea2", "ntfs", "Recovery", NULL);
    part(t, "nvme0n1", "nvme0n1", "Corsair_MP700", 5, 1621524480ull, 1168231104512ull, LINUX, "0431973a-f38f-47d9-899d-d81092489fe0", "ext4", NULL, NULL);
    part(t, "nvme0n1", "nvme0n1", "Corsair_MP700", 6, 3903225856ull, 1074789888, ESP, "B10A0A39-9869-4193-A715-94AEA4CF86E5", "vfat", NULL, NULL);

    drive(t, "Samsung_9100", "00coldplug/00fixed/nvme1n1", "", 0);
    whole(t, "nvme1n1", "Samsung_9100", 4000787030016ull, "gpt");
    part(t, "nvme1n1", "nvme1n1", "Samsung_9100", 1, 2048, 1127219200, ESP, "2d5d704b-c27b-4835-b33e-9c9b125a0b1d", "vfat", NULL, "/boot/efi");
    part(t, "nvme1n1", "nvme1n1", "Samsung_9100", 2, 2203648, 64000884736ull, SWAP, "938676b6-4327-4a79-a15a-94df4c5e53c5", "swap", NULL, NULL);
    part(t, "nvme1n1", "nvme1n1", "Samsung_9100", 3, 127205376, 3935655952384ull, LINUX, "c5193af0-0ae0-44bb-83eb-e99dc32e7d00", "ext4", NULL, "/");

    /* a stick: one FAT partition at 1 MiB, the rest of its 32 GB free */
    drive(t, "SanDisk", "01hotplug/sda", "usb", 1);
    whole(t, "sda", "SanDisk", 32000000000ull, "dos");
    part(t, "sda", "sda", "SanDisk", 1, 2048, 16 * GIB, "0x0c", "1234-ABCD", "vfat", "STICK", NULL);

    /* a USB disk with Fedora mounted, its swap after it, a 4 MiB gap (hidden) and an unknown partition */
    drive(t, "WD_Passport", "01hotplug/sdb", "usb", 0);
    whole(t, "sdb", "WD_Passport", 1000 * GIB, "gpt");
    part(t, "sdb", "sdb", "WD_Passport", 1, 2048, 100 * GIB, LINUX, "aaaaaaaa-0000-0000-0000-000000000001", "btrfs", "fedora", fedora_root);
    part(t, "sdb", "sdb", "WD_Passport", 2, 2048 + 100 * GIB / 512, 8 * GIB, SWAP, "aaaaaaaa-0000-0000-0000-000000000002", "swap", NULL, NULL);
    part(t, "sdb", "sdb", "WD_Passport", 3, 2048 + 108 * GIB / 512 + 8192, 50 * GIB, "48465300-0000-11aa-aa11-00306543ecac",
         "aaaaaaaa-0000-0000-0000-000000000003", "hfsplus", NULL, NULL);

    static const char *const fs[] = {"btrfs", "ext4", "xfs"};
    for (int i = 0; i < 3; i++)
        FR_PUSH(t->resize, ((dk_resize_mode){fr_strdup(fs[i]), DK_OFFLINE_SHRINK | DK_OFFLINE_GROW | DK_ONLINE_GROW}));
    FR_PUSH(t->resize, ((dk_resize_mode){fr_strdup("vfat"), DK_OFFLINE_SHRINK | DK_OFFLINE_GROW}));
}

/* ---- EFI load options ---- */

static void guid_le(const char *s, unsigned char out[16])
{
    unsigned a, b, c, d[8];
    sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
    out[0] = a, out[1] = a >> 8, out[2] = a >> 16, out[3] = a >> 24, out[4] = b, out[5] = b >> 8, out[6] = c, out[7] = c >> 8;
    for (int i = 0; i < 8; i++) out[8 + i] = d[i];
}

static size_t load_option(unsigned char *buf, const char *name, int active, const char *part_uuid)
{
    size_t n = 0;
    buf[n++] = 7, buf[n++] = 0, buf[n++] = 0, buf[n++] = 0;
    buf[n++] = active, buf[n++] = 0, buf[n++] = 0, buf[n++] = 0;
    size_t len_at = n;
    n += 2;
    for (const char *c = name; *c; c++) buf[n++] = (unsigned char)*c, buf[n++] = 0;
    buf[n++] = 0, buf[n++] = 0;
    size_t path_start = n;
    if (part_uuid) {
        unsigned char hd[42] = {0x04, 0x01, 42, 0};
        guid_le(part_uuid, hd + 24);
        hd[40] = 0x02, hd[41] = 0x02;
        memcpy(buf + n, hd, sizeof hd);
        n += sizeof hd;
        static const unsigned char file[] = {0x04, 0x04, 8, 0, '\\', 0, 0, 0};
        memcpy(buf + n, file, sizeof file);
        n += sizeof file;
    }
    buf[n++] = 0x7f, buf[n++] = 0xff, buf[n++] = 4, buf[n++] = 0;
    buf[len_at] = (unsigned char)(n - path_start), buf[len_at + 1] = (unsigned char)((n - path_start) >> 8);
    return n;
}

static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fwrite(data, 1, n, f);
    fclose(f);
}

static void test_boot_entries(dk_env *env)
{
    unsigned char buf[512];
    dk_boot b;
    size_t n = load_option(buf, "Pop!_OS 24.04 LTS", 1, "b10a0a39-9869-4193-a715-94aea4cf86e5");
    CHECK(!dk_boot_parse(buf, n, &b));
    CHECK_STR(b.name, "Pop!_OS 24.04 LTS");
    CHECK_STR(b.part_uuid, "b10a0a39-9869-4193-a715-94aea4cf86e5");
    CHECK(b.active == 1);
    free(b.name), free(b.part_uuid);
    n = load_option(buf, "UEFI:Network Device", 1, NULL);
    CHECK(dk_boot_parse(buf, n, &b) == -1);
    CHECK(dk_boot_parse(buf, 6, &b) == -1);

    char *dir = fr_format("%s/efivars", tmp);
    mkdir(dir, 0700);
    static const struct {
        const char *var, *name, *uuid;
        int active;
    } vars[] = {
        {"Boot0000", "Windows Boot Manager", "35c657cf-626b-43e0-bd89-ce998645a441", 1},
        {"Boot0001", "Pop!_OS 24.04 LTS", "b10a0a39-9869-4193-a715-94aea4cf86e5", 1},
        {"Boot0003", "Ubuntu", "2d5d704b-c27b-4835-b33e-9c9b125a0b1d", 1},
        {"Boot0004", "ubuntu", "35c657cf-626b-43e0-bd89-ce998645a441", 0},
        {"Boot0005", "UEFI OS", "b10a0a39-9869-4193-a715-94aea4cf86e5", 0},
        {"Boot0006", "UEFI:CD/DVD Drive", NULL, 1},
        {"Bootabcd", "lower-case hex is not a Boot#### variable", "35c657cf-626b-43e0-bd89-ce998645a441", 1},
    };
    for (size_t i = 0; i < sizeof vars / sizeof *vars; i++) {
        char *path = fr_format("%s/%s-8be4df61-93ca-11d2-aa0d-00e098032b8c", dir, vars[i].var);
        write_file(path, buf, load_option(buf, vars[i].name, vars[i].active, vars[i].uuid));
        free(path);
    }
    dk_boot_read(dir, env);
    CHECK(env->boot.n == 5);
    free(dir);

    char *swaps = fr_format("%s/swaps", tmp);
    const char *text = "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n/swap.img                               file\t\t8388604\t\t388\t\t-1\n";
    write_file(swaps, text, strlen(text));
    dk_swaps_read(swaps, &env->swaps);
    CHECK(env->swaps.n == 1 && !strcmp(env->swaps.v[0], "/swap.img"));
    free(swaps);
}

/* ---- the model ---- */

static const dk_seg *seg(const dk_disks *d, const char *disk, const char *dev)
{
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->v[i].device, disk))
            for (int k = 0; k < d->v[i].segs.n; k++)
                if (d->v[i].segs.v[k].device && !strcmp(d->v[i].segs.v[k].device, dev)) return &d->v[i].segs.v[k];
    return NULL;
}

static const char *disk_path(const dk_disks *d, const char *dev)
{
    for (int i = 0; i < d->n; i++)
        if (!strcmp(d->v[i].device, dev)) return d->v[i].path;
    return "";
}

static void test_model(const dk_disks *d)
{
    CHECK(d->n == 4);
    if (d->n != 4) return;
    CHECK_STR(d->v[0].device, "/dev/nvme0n1");
    CHECK_STR(d->v[1].device, "/dev/nvme1n1");
    CHECK_STR(d->v[2].device, "/dev/sda");
    CHECK_STR(d->v[3].device, "/dev/sdb");
    CHECK(!d->v[0].root_disk && d->v[1].root_disk && !d->v[2].root_disk);
    CHECK_STR(d->v[0].name, "Corsair_MP700");
    CHECK_STR(d->v[0].table_type, "gpt");

    /* in disk order, the trailing 88 KiB left over is no free space */
    const dk_segs *s = &d->v[0].segs;
    CHECK(s->n == 6);
    static const char *const order[] = {"/dev/nvme0n1p1", "/dev/nvme0n1p2", "/dev/nvme0n1p3", "/dev/nvme0n1p5", "/dev/nvme0n1p6", "/dev/nvme0n1p4"};
    for (int i = 0; i < 6 && i < s->n; i++) CHECK_STR(s->v[i].device, order[i]);

    const dk_seg *x = seg(d, "/dev/nvme0n1", "/dev/nvme0n1p1");
    CHECK(x && !strcmp(x->group, "Windows") && x->group_kind == GROUP_WINDOWS && !x->inferred);
    CHECK(x && !strcmp(x->type_name, "EFI system") && !strcmp(dk_seg_name(x), "SYSTEM"));
    x = seg(d, "/dev/nvme0n1", "/dev/nvme0n1p2");
    CHECK(x && !strcmp(x->group, "Windows") && !strcmp(x->type_name, "Microsoft reserved"));
    x = seg(d, "/dev/nvme0n1", "/dev/nvme0n1p3");
    CHECK(x && !strcmp(x->group, "Windows") && x->encrypted);
    x = seg(d, "/dev/nvme0n1", "/dev/nvme0n1p4");
    CHECK(x && !strcmp(x->group, "Windows"));
    /* the ESP from its active boot entry (the UEFI OS fallback loader does not name it); the unmounted ext4 beside
     * it takes its OS, marked inferred, and not from the Windows side */
    x = seg(d, "/dev/nvme0n1", "/dev/nvme0n1p6");
    CHECK(x && !strcmp(x->group, "Pop!_OS 24.04 LTS") && x->group_kind == GROUP_OS && !x->inferred);
    CHECK(x && !strcmp(x->part_uuid, "b10a0a39-9869-4193-a715-94aea4cf86e5"));
    x = seg(d, "/dev/nvme0n1", "/dev/nvme0n1p5");
    CHECK(x && !strcmp(x->group, "Pop!_OS 24.04 LTS") && x->inferred);

    s = &d->v[1].segs;
    CHECK(s->n == 3);
    x = seg(d, "/dev/nvme1n1", "/dev/nvme1n1p1");
    CHECK(x && !strcmp(x->group, "Ubuntu 24.04.5 LTS") && x->group_kind == GROUP_RUNNING && x->system_mount);
    /* swap there is off (a swap file is active): its OS is only inferred */
    x = seg(d, "/dev/nvme1n1", "/dev/nvme1n1p2");
    CHECK(x && x->group_kind == GROUP_RUNNING && x->inferred && !x->swap_active);
    x = seg(d, "/dev/nvme1n1", "/dev/nvme1n1p3");
    CHECK(x && x->group_kind == GROUP_RUNNING && !x->inferred && x->has_used && x->resize_modes & DK_ONLINE_GROW);

    s = &d->v[2].segs;
    CHECK(s->n == 2 && s->v[1].kind == SEG_FREE && s->v[1].offset == DK_MIB + 16 * GIB);
    CHECK(s->n == 2 && s->v[0].group_kind == GROUP_UNKNOWN && !strcmp(s->v[0].group, "Unidentified"));

    /* os-release of a mounted root, its swap inferred, the 4 MiB gap hidden, HFS+ unidentified */
    s = &d->v[3].segs;
    CHECK(s->n == 4);
    x = seg(d, "/dev/sdb", "/dev/sdb1");
    CHECK(x && !strcmp(x->group, "Fedora Linux 41 (Workstation Edition)") && x->group_kind == GROUP_OS && !x->inferred);
    x = seg(d, "/dev/sdb", "/dev/sdb2");
    CHECK(x && !strcmp(x->group, "Fedora Linux 41 (Workstation Edition)") && x->inferred);
    x = seg(d, "/dev/sdb", "/dev/sdb3");
    CHECK(x && x->group_kind == GROUP_UNKNOWN && !strcmp(x->type_name, "Apple HFS+"));
    CHECK(s->n == 4 && s->v[3].kind == SEG_FREE);
}

/* ---- the plan ---- */

static dk_disks build(const char *fedora_root, int swap_on)
{
    dk_tree t = {0};
    machine(&t, fedora_root);
    if (swap_on)
        for (int i = 0; i < t.objs.n; i++)
            if (t.objs.v[i].device && !strcmp(t.objs.v[i].device, "/dev/nvme1n1p2")) t.objs.v[i].swap_active = 1;
    dk_env env = {0};
    env.running = fr_strdup("Ubuntu 24.04.5 LTS");
    FR_PUSH(env.swaps, fr_strdup("/swap.img"));
    dk_boot_read(tmp, &env);
    char *efivars = fr_format("%s/efivars", tmp);
    dk_boot_read(efivars, &env);
    free(efivars);
    dk_disks d = {0};
    dk_build(&t, &env, &d);
    dk_env_free(&env);
    dk_tree_free(&t);
    return d;
}

static const char *key_of(const dk_plan *p, const char *disk, const char *dev)
{
    const dk_seg *s = seg(&p->disks, disk, dev);
    return s ? s->key : "";
}

static void test_plan(const char *fedora_root)
{
    dk_plan p = {0};
    dk_disks base = build(fedora_root, 1);
    dk_plan_set_base(&p, &base);
    char *nvme0 = fr_strdup(disk_path(&p.disks, "/dev/nvme0n1")), *nvme1 = fr_strdup(disk_path(&p.disks, "/dev/nvme1n1"));
    char *sda = fr_strdup(disk_path(&p.disks, "/dev/sda")), *sdb = fr_strdup(disk_path(&p.disks, "/dev/sdb"));

    /* the refusals, per partition */
    CHECK_STR(dk_plan_stage_delete(&p, nvme1, key_of(&p, "/dev/nvme1n1", "/dev/nvme1n1p3")), "In use by the running system.");
    CHECK_STR(dk_plan_stage_format(&p, nvme1, key_of(&p, "/dev/nvme1n1", "/dev/nvme1n1p1"), "vfat", ""), "In use by the running system.");
    CHECK_STR(dk_plan_stage_delete(&p, nvme1, key_of(&p, "/dev/nvme1n1", "/dev/nvme1n1p2")), "In use as swap.");
    CHECK_STR(dk_plan_stage_format(&p, nvme0, key_of(&p, "/dev/nvme0n1", "/dev/nvme0n1p3"), "ext4", ""), "Encrypted: read-only here.");
    CHECK_STR(dk_plan_stage_delete(&p, sdb, key_of(&p, "/dev/sdb", "/dev/sdb1")), "Mounted: unmount it first.");
    CHECK_STR(dk_plan_stage_label(&p, nvme0, key_of(&p, "/dev/nvme0n1", "/dev/nvme0n1p2"), "x"), "No filesystem to label.");
    CHECK_STR(dk_plan_stage_delete(&p, nvme0, "free:0"), "That partition no longer exists.");
    CHECK_STR(dk_plan_blocked(&p, nvme1, key_of(&p, "/dev/nvme1n1", "/dev/nvme1n1p3"), OP_RESIZE),
              "Mounted, so it can only grow, and there is no free space after it.");
    CHECK_STR(dk_plan_blocked(&p, sdb, key_of(&p, "/dev/sdb", "/dev/sdb3"), OP_RESIZE), "UDisks2 cannot resize hfsplus here.");
    CHECK_STR(dk_plan_stage_image(&p, nvme0, "/nonexistent.iso"), "Cannot read /nonexistent.iso.");
    CHECK(p.ops.n == 0);
    /* the Windows side cannot shrink (BitLocker); the Pop!_OS partition can */
    CHECK(dk_plan_blocked(&p, nvme0, key_of(&p, "/dev/nvme0n1", "/dev/nvme0n1p5"), OP_RESIZE) == NULL);
    CHECK(dk_plan_blocked(&p, nvme0, key_of(&p, "/dev/nvme0n1", "/dev/nvme0n1p5"), OP_DELETE) == NULL);

    /* shrink Pop!_OS, then make a partition in the space it gave */
    uint64_t min, max;
    const char *p5 = "/org/freedesktop/UDisks2/block_devices/nvme0n1p5";
    CHECK(dk_plan_limits(&p, nvme0, p5, &min, &max) && min == DK_MIB && max == 1168231104512ull / DK_MIB * DK_MIB);
    CHECK(dk_plan_stage_resize(&p, nvme0, p5, 600 * GIB + 12345) == NULL);
    const dk_segs *s = &p.disks.v[0].segs;
    CHECK(s->n == 7 && s->v[3].size == 600 * GIB && s->v[3].change == CHANGE_RESIZE && s->v[4].kind == SEG_FREE);
    CHECK(s->n == 7 && s->v[4].size == 1168231104512ull - 600 * GIB);
    CHECK(dk_plan_stage_create(&p, nvme0, s->v[4].offset, 200 * GIB, "ext4", "data") == NULL);
    s = &p.disks.v[0].segs;
    CHECK(s->n == 8 && s->v[4].change == CHANGE_CREATE && s->v[4].group_kind == GROUP_NEW && s->v[5].kind == SEG_FREE);
    CHECK(s->n == 8 && !strcmp(s->v[4].key, "new:1") && !strcmp(s->v[4].label, "data"));
    /* staged changes to a new partition fold into its create */
    CHECK(dk_plan_stage_format(&p, nvme0, "new:1", "xfs", "scratch") == NULL);
    CHECK(p.ops.n == 2 && !strcmp(p.ops.v[1].fs_type, "xfs") && !strcmp(p.ops.v[1].label, "scratch"));
    /* a second resize of the same partition replaces the first */
    CHECK(dk_plan_stage_resize(&p, nvme0, p5, 500 * GIB) == NULL);
    CHECK(p.ops.n == 2 && p.ops.v[0].size == 500 * GIB);
    CHECK_STR(p.ops.v[0].text, "Resize /dev/nvme0n1p5 from 1.1 TiB to 500.0 GiB");
    CHECK_STR(p.ops.v[1].text, "Create 200.0 GiB xfs partition \"scratch\"");

    /* sticks only, and only real boot images */
    char *bad = fr_format("%s/windows.iso", tmp), *good = fr_format("%s/linux.iso", tmp);
    unsigned char img[2 * 512] = {0};
    write_file(bad, img, sizeof img);
    img[510] = 0x55, img[511] = 0xAA;
    write_file(good, img, sizeof img);
    CHECK(strstr(dk_plan_stage_image(&p, sda, bad), "has no boot record") != NULL);
    CHECK_STR(dk_plan_stage_image(&p, nvme0, good), "Images go only onto removable or USB disks.");
    CHECK_STR(dk_plan_stage_image(&p, nvme1, good), "The running system lives on this disk.");
    CHECK_STR(dk_plan_stage_image(&p, sdb, good), "A partition on this disk is mounted: unmount it first.");
    CHECK(dk_plan_stage_image(&p, sda, good) == NULL);
    s = &p.disks.v[2].segs;
    CHECK(p.disks.v[2].imaged && s->n == 2 && s->v[0].kind == SEG_IMAGE && s->v[1].kind == SEG_FREE);
    CHECK_STR(dk_plan_stage_delete(&p, sda, "image"), "This disk is being overwritten by an image.");
    CHECK_STR(dk_plan_blocked(&p, sda, "image", OP_FORMAT), "This disk is being overwritten by an image.");

    char words[64];
    dk_plan_confirm_words(&p, words, sizeof words);
    CHECK_STR(words, "nvme0n1 sda");

    /* the calls: shrink the filesystem before the partition; create and format in one call on the disk */
    dk_steps steps = {0};
    CHECK(dk_plan_begin(&p, &steps) && steps.n == 3 && p.running);
    CHECK(steps.n == 3 && steps.v[0].n == 2 && steps.v[0].v[0].method == CALL_FS_RESIZE && steps.v[0].v[1].method == CALL_PART_RESIZE);
    CHECK(steps.n == 3 && steps.v[0].v[0].size == 500 * GIB && steps.v[0].v[1].size == 500 * GIB);
    CHECK(steps.n == 3 && steps.v[1].n == 1 && steps.v[1].v[0].method == CALL_CREATE_FORMAT);
    CHECK(steps.n == 3 && !strcmp(steps.v[1].v[0].object, BLOCKS "nvme0n1") && !strcmp(steps.v[1].v[0].ptype, LINUX));
    CHECK(steps.n == 3 && !strcmp(steps.v[1].v[0].fs, "xfs") && steps.v[1].v[0].size == 200 * GIB);
    CHECK(steps.n == 3 && steps.v[2].v[0].method == CALL_IMAGE && !strcmp(steps.v[2].v[0].object, BLOCKS "sda"));
    CHECK_STR(dk_call_method(CALL_IMAGE), "OpenDevice");
    dk_steps_free(&steps);
    CHECK_STR(dk_plan_stage_delete(&p, nvme0, p5), "Wait for the running changes to finish.");
    dk_plan_finish(&p, "Not authorized");
    CHECK(!p.running && p.ops.n == 3 && p.ops.v[0].type == OP_RESIZE);
    CHECK(strstr(p.error, "Resize /dev/nvme0n1p5 from 1.1 TiB to 500.0 GiB failed: Not authorized") == p.error);
    CHECK(dk_plan_begin(&p, &steps) && steps.n == 3);
    dk_steps_free(&steps);
    dk_plan_step_done(&p);
    CHECK(p.done == 1 && p.running);
    /* the done resize never reached this fake base, so the create after it no longer fits and drops too */
    dk_plan_finish(&p, "Not authorized");
    CHECK(!p.running && p.ops.n == 1 && p.ops.v[0].type == OP_IMAGE && strstr(p.error, "Dropped, the disk changed: Create") == p.error);

    /* growing: the partition first, then the filesystem to fill it */
    dk_plan_clear(&p);
    CHECK(p.ops.n == 0 && !p.error[0]);
    CHECK(dk_plan_stage_delete(&p, nvme0, key_of(&p, "/dev/nvme0n1", "/dev/nvme0n1p5")) == NULL);
    CHECK(dk_plan_stage_create(&p, nvme0, p.disks.v[0].segs.v[3].offset, 100 * GIB, "", "") == NULL);
    s = &p.disks.v[0].segs;
    CHECK(s->n == 7 && s->v[3].change == CHANGE_CREATE && s->v[4].kind == SEG_FREE);
    CHECK(dk_plan_stage_resize(&p, nvme0, "new:2", 300 * GIB) == NULL);
    CHECK(p.ops.n == 2 && p.ops.v[1].size == 300 * GIB);
    CHECK(dk_plan_begin(&p, &steps) && steps.n == 2 && steps.v[0].v[0].method == CALL_DELETE && steps.v[1].v[0].method == CALL_CREATE);
    dk_steps_free(&steps);
    dk_plan_finish(&p, NULL);
    CHECK(!p.running && p.ops.n == 0);

    /* swap has no resize tool; a partition without a filesystem resizes alone */
    CHECK_STR(dk_plan_stage_resize(&p, sdb, key_of(&p, "/dev/sdb", "/dev/sdb2"), 20 * GIB), "UDisks2 cannot resize swap here.");
    CHECK(dk_plan_stage_resize(&p, nvme0, key_of(&p, "/dev/nvme0n1", "/dev/nvme0n1p2"), 8 * DK_MIB) == NULL);
    CHECK(dk_plan_begin(&p, &steps) && steps.n == 1 && steps.v[0].n == 1 && steps.v[0].v[0].method == CALL_PART_RESIZE);
    dk_steps_free(&steps);
    dk_plan_finish(&p, NULL);

    /* removing a step drops what depended on it */
    CHECK(dk_plan_stage_delete(&p, nvme0, p5) == NULL);
    CHECK(dk_plan_stage_create(&p, nvme0, p.disks.v[0].segs.v[3].offset, 10 * GIB, "ext4", "") == NULL);
    dk_plan_remove(&p, 0);
    CHECK(p.ops.n == 0 && strstr(p.error, "Also dropped, it depended on that step: Create 10.0 GiB ext4 partition: ") == p.error);

    /* the disks changed under the queue: what no longer applies is dropped */
    CHECK(dk_plan_stage_label(&p, nvme0, p5, "pop") == NULL);
    CHECK(dk_plan_stage_label(&p, sda, key_of(&p, "/dev/sda", "/dev/sda1"), "boot") == NULL);
    dk_disks changed = build(fedora_root, 1);
    dk_seg *gone = (dk_seg *)seg(&changed, "/dev/nvme0n1", "/dev/nvme0n1p5");
    free(gone->key), gone->key = fr_strdup("free:x");
    dk_plan_set_base(&p, &changed);
    CHECK(p.ops.n == 1 && strstr(p.error, "Dropped, the disk changed: Label /dev/nvme0n1p5 \"pop\": That partition no longer exists.") == p.error);
    dk_plan_free(&p);
    free(bad), free(good), free(nvme0), free(nvme1), free(sda), free(sdb);
}

static void test_refusal(const char *fedora_root)
{
    dk_tree t = {0};
    machine(&t, fedora_root);
    char *root = dk_root_drive(&t.objs);
    CHECK_STR(root, DRIVES "Samsung_9100");
    CHECK_STR(dk_refusal(&t.objs, root, BLOCKS "nvme1n1", "OpenDevice"), "Refused: the running system lives on this disk.");
    CHECK_STR(dk_refusal(&t.objs, root, DRIVES "Samsung_9100", "PowerOff"), "Refused: the running system lives on this disk.");
    CHECK_STR(dk_refusal(&t.objs, root, BLOCKS "sdb1", "Format"), "Refused: it is in use.");
    CHECK_STR(dk_refusal(&t.objs, root, BLOCKS "nvme1n1p1", "Unmount"), "Refused: the running system needs it.");
    CHECK(dk_refusal(&t.objs, root, BLOCKS "sdb1", "Unmount") == NULL);
    CHECK(dk_refusal(&t.objs, root, BLOCKS "sda", "OpenDevice") == NULL);
    CHECK(dk_refusal(&t.objs, root, BLOCKS "nvme1n1p3", "Resize") == NULL);
    free(root);
    dk_tree_free(&t);
}

static void test_sizes(void)
{
    CHECK(dk_parse_size("200G") == (int64_t)(200 * GIB));
    CHECK(dk_parse_size(" 1.5 TiB ") == (int64_t)(1536 * GIB));
    CHECK(dk_parse_size("512 MiB") == (int64_t)(512 * DK_MIB));
    CHECK(dk_parse_size("4096") == 4096);
    CHECK(dk_parse_size("2 kb") == 2048);
    CHECK(dk_parse_size("abc") == -1 && dk_parse_size("12 x") == -1 && dk_parse_size("") == -1 && dk_parse_size("1e3") == -1);
    char b[32];
    dk_size_text(1168231104512ull, b, sizeof b);
    CHECK_STR(b, "1.1 TiB");
    dk_size_text(512 * DK_MIB, b, sizeof b);
    CHECK_STR(b, "512.0 MiB");
    dk_size_text(900, b, sizeof b);
    CHECK_STR(b, "900 B");
}

/* ---- look inside ---- */

static void fill(const char *path, size_t bytes)
{
    char *data = calloc(1, bytes ? bytes : 1);
    memset(data, 'x', bytes);
    write_file(path, data, bytes);
    free(data);
}

static char *wait_scan(void)
{
    for (int i = 0; i < 500; i++) {
        char *k = dk_usage_poll();
        if (k) return k;
        usleep(10000);
    }
    return NULL;
}

static void test_usage(void)
{
    char *vol = fr_format("%s/vol", tmp), *p;
    mkdir(vol, 0700);
#define AT(...) (free(p), p = fr_format(__VA_ARGS__))
    p = NULL;
    mkdir(AT("%s/big", vol), 0700);
    mkdir(AT("%s/big/deep", vol), 0700);
    fill(AT("%s/big/deep/a.bin", vol), 3 << 20);
    fill(AT("%s/big/b.bin", vol), 1 << 20);
    mkdir(AT("%s/many", vol), 0700);
    for (int i = 0; i < 25; i++) fill(AT("%s/many/f%02d", vol, i), 4096 * (size_t)(i + 1));
    mkdir(AT("%s/locked", vol), 0700);
    fill(AT("%s/top.bin", vol), 512 << 10);
    char *target = fr_strdup(p);
    link(target, AT("%s/top-link.bin", vol));
    chmod(AT("%s/locked", vol), 0);

    dk_scan *s = dk_usage_start("blk", "Vol", vol);
    unsigned id = s->id;
    CHECK(s->status == SCAN_SCANNING && !strcmp(dk_usage_active(), "blk"));
    char *done = wait_scan();
    CHECK(done && !strcmp(done, "blk"));
    free(done);
    CHECK(s->status == SCAN_DONE && !dk_usage_active());
    /* 2 + 25 + 1 files; the hard link counted once */
    CHECK(s->files == 28);
    dk_rows rows = {0};
    dk_usage_rows(s, NULL, &rows);
    CHECK(rows.n >= 4);
    CHECK(rows.n >= 4 && !strcmp(rows.v[0].name, "big") && rows.v[0].kind == ROW_DIR && rows.v[0].files == 2);
    int locked = 0, top = 0, many = -1;
    for (int i = 0; i < rows.n; i++) {
        locked |= !strcmp(rows.v[i].name, "locked") && rows.v[i].kind == ROW_DENIED;
        top += !strncmp(rows.v[i].name, "top", 3) && rows.v[i].kind == ROW_FILE;
        if (!strcmp(rows.v[i].name, "many")) many = rows.v[i].index;
        if (i) CHECK(rows.v[i - 1].kind > ROW_DENIED || rows.v[i].kind > ROW_DENIED || rows.v[i - 1].size >= rows.v[i].size);
    }
    CHECK(locked && top == 1 && many >= 0);
    CHECK(rows.here == s->root->size && rows.files == 28);
    /* past the 20 largest, files are summed */
    if (many >= 0) {
        const dk_node *m = s->root->kids[many];
        dk_usage_rows(s, m, &rows);
        CHECK(rows.n == 21 && rows.v[20].kind == ROW_OTHER && !strcmp(rows.v[20].name, "5 smaller files"));
        CHECK(rows.v[0].size >= rows.v[1].size && !strcmp(rows.v[0].name, "f24"));
    }
    /* kept for the session; a rescan is a new scan */
    CHECK(dk_usage_find("blk") == s);
    s = dk_usage_start("blk", "Vol", vol);
    CHECK(s->id != id);
    free(wait_scan());
    char *missing = fr_format("%s/missing", tmp);
    s = dk_usage_start("gone", "Gone", missing);
    free(wait_scan());
    CHECK(s->status == SCAN_ERROR && strstr(s->error, "Cannot read") == s->error);
    dk_scan *m = dk_usage_mounting("other", "Other");
    CHECK(m->status == SCAN_MOUNTING && !strcmp(dk_usage_active(), "other"));
    dk_usage_fail("other", "Not authorized");
    CHECK(m->status == SCAN_ERROR && !dk_usage_active());
    dk_usage_forget();
    CHECK(!dk_usage_find("blk") && !dk_usage_find("other"));
    chmod(AT("%s/locked", vol), 0700);
    free(rows.v), free(missing), free(target), free(p), free(vol);
#undef AT
}

/* ---- image writing, into a plain file ---- */

static void test_image(void)
{
    dk_sha256 h;
    unsigned char out[32];
    dk_sha256_init(&h);
    dk_sha256_add(&h, "abc", 3);
    dk_sha256_end(&h, out);
    static const unsigned char abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                          0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    CHECK(!memcmp(out, abc, 32));

    char *img = fr_format("%s/hybrid.img", tmp), *dev = fr_format("%s/stick", tmp);
    size_t n = 5 * 1024 * 1024 + 123;
    unsigned char *data = malloc(n);
    for (size_t i = 0; i < n; i++) data[i] = (unsigned char)(i * 7 + i / 4096);
    data[510] = 0x55, data[511] = 0xAA;
    write_file(img, data, n);
    char why[256];
    CHECK(dk_image_check(img, why, sizeof why) == 0);
    int fd = open(dev, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    char *err = dk_image_write(fd, img, n, NULL, NULL);
    CHECK(err == NULL);
    free(err);
    FILE *f = fopen(dev, "rb");
    unsigned char *back = malloc(n + 1);
    CHECK(f && fread(back, 1, n + 1, f) == n && !memcmp(back, data, n));
    if (f) fclose(f);
    fd = open(dev, O_RDWR);
    err = dk_image_write(fd, img, n + 1, NULL, NULL);
    CHECK(err && strstr(err, "Reading"));
    free(err);
    free(data), free(back), free(img), free(dev);
}

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw) { return remove(path); }

int main(void)
{
    snprintf(tmp, sizeof tmp, "/tmp/ferret-disks-XXXXXX");
    if (!mkdtemp(tmp)) return 1;
    char *fedora = fr_format("%s/fedora", tmp), *etc = fr_format("%s/etc", fedora), *rel = fr_format("%s/os-release", etc);
    mkdir(fedora, 0700), mkdir(etc, 0700);
    const char *text = "NAME=\"Fedora Linux\"\nPRETTY_NAME=\"Fedora Linux 41 (Workstation Edition)\"\n";
    write_file(rel, text, strlen(text));

    dk_env env = {0};
    test_boot_entries(&env);
    dk_env_free(&env);
    dk_disks d = build(fedora, 0);
    test_model(&d);
    dk_disks_free(&d);
    test_plan(fedora);
    test_refusal(fedora);
    test_sizes();
    test_usage();
    test_image();
    free(fedora), free(etc), free(rel);
    nftw(tmp, remove_one, 16, FTW_DEPTH | FTW_PHYS);
    return CHECK_DONE();
}

#define _GNU_SOURCE
#include "plan.h"
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int eq(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static int empty(const char *s) { return !s || !*s; }
static char *dup_or_null(const char *s) { return s ? fr_strdup(s) : NULL; }
static uint64_t align_down(uint64_t v) { return v / DK_MIB * DK_MIB; }
static uint64_t align_up(uint64_t v) { return (v + DK_MIB - 1) / DK_MIB * DK_MIB; }
static int is_new(const char *key) { return key && !strncmp(key, "new:", 4); }

static char *size_text(uint64_t bytes)
{
    char b[32];
    dk_size_text(bytes, b, sizeof b);
    return fr_strdup(b);
}

static const char *base_name(const char *path)
{
    const char *s = path ? strrchr(path, '/') : NULL;
    return s ? s + 1 : path ? path : "";
}

/* ---- ops ---- */

static void op_free(dk_op *o)
{
    free(o->disk), free(o->target), free(o->fs_type), free(o->label), free(o->file), free(o->text);
    *o = (dk_op){0};
}

static dk_op op_copy(const dk_op *o)
{
    dk_op c = *o;
    c.disk = dup_or_null(o->disk), c.target = dup_or_null(o->target), c.fs_type = dup_or_null(o->fs_type);
    c.label = dup_or_null(o->label), c.file = dup_or_null(o->file), c.text = dup_or_null(o->text);
    return c;
}

static void ops_free(dk_ops *o)
{
    for (int i = 0; i < o->n; i++) op_free(&o->v[i]);
    free(o->v);
    *o = (dk_ops){0};
}

static dk_ops ops_copy(const dk_ops *o, int from)
{
    dk_ops c = {0};
    for (int i = from; i < o->n; i++) FR_PUSH(c, op_copy(&o->v[i]));
    return c;
}

static void ops_remove(dk_ops *o, int i)
{
    op_free(&o->v[i]);
    memmove(o->v + i, o->v + i + 1, sizeof *o->v * (size_t)(o->n - i - 1));
    o->n--;
}

/* ---- projecting one op ---- */

static const char *short_device(const dk_seg *s) { return s->kind == SEG_FREE ? "free space" : s->device ? s->device : ""; }

static void segs_remove(dk_segs *s, int i)
{
    dk_seg_free(&s->v[i]);
    memmove(s->v + i, s->v + i + 1, sizeof *s->v * (size_t)(s->n - i - 1));
    s->n--;
}

static void segs_insert(dk_segs *s, int i, dk_seg seg)
{
    FR_GROW(*s, s->n + 1);
    memmove(s->v + i + 1, s->v + i, sizeof *s->v * (size_t)(s->n - i));
    s->v[i] = seg;
    s->n++;
}

/* merges neighbouring free gaps and drops what is left under DK_MIN_FREE, as partitioning tools align */
static void tidy(dk_segs *s)
{
    for (int i = 1; i < s->n;) {
        if (s->v[i].kind == SEG_FREE && s->v[i - 1].kind == SEG_FREE) {
            dk_seg merged = dk_free_seg(s->v[i - 1].offset, s->v[i - 1].size + s->v[i].size);
            dk_seg_free(&s->v[i - 1]);
            s->v[i - 1] = merged;
            segs_remove(s, i);
        } else i++;
    }
    for (int i = 0; i < s->n;) {
        if (s->v[i].kind == SEG_FREE && s->v[i].size < DK_MIN_FREE) segs_remove(s, i);
        else i++;
    }
}

static char reason_buf[128];

static const char *reason(const dk_seg *s, int action)
{
    if (s->kind == SEG_IMAGE) return "Part of the image being written to this disk.";
    if (s->kind == SEG_FREE) return action == OP_CREATE ? NULL : "This is free space.";
    if (action == OP_CREATE) return "Not free space.";
    if (s->swap_active) return "In use as swap.";
    int mounted = s->mounts.n > 0;
    /* a mounted filesystem may only grow, and only where it can grow online (ext4, btrfs, xfs) */
    if (mounted && !(action == OP_RESIZE && (s->resize_modes & DK_ONLINE_GROW)))
        return s->system_mount ? "In use by the running system." : "Mounted: unmount it first.";
    if (s->encrypted) return "Encrypted: read-only here.";
    if (s->kind == SEG_FILESYSTEM && (action == OP_DELETE || action == OP_RESIZE)) return "The whole disk is one filesystem, not a partition.";
    if (action == OP_LABEL && (!s->has_fs || eq(s->fs_type, "swap"))) return "No filesystem to label.";
    if (action == OP_RESIZE && !mounted && !(s->resize_modes & (DK_OFFLINE_SHRINK | DK_OFFLINE_GROW))) {
        if (empty(s->fs_type)) return "Cannot resize.";
        snprintf(reason_buf, sizeof reason_buf, "UDisks2 cannot resize %s here.", s->fs_type);
        return reason_buf;
    }
    return NULL;
}

static void limits(const dk_segs *segs, int i, uint64_t *min, uint64_t *max)
{
    const dk_seg *s = &segs->v[i];
    int mounted = s->mounts.n > 0;
    int shrink = !mounted && (s->resize_modes & DK_OFFLINE_SHRINK);
    int grow = mounted ? (s->resize_modes & DK_ONLINE_GROW) != 0 : (s->resize_modes & DK_OFFLINE_GROW) != 0;
    uint64_t after = i + 1 < segs->n && segs->v[i + 1].kind == SEG_FREE ? segs->v[i + 1].size : 0;
    uint64_t floor = s->has_used ? align_up(s->used + s->used / 10) : DK_MIB;
    *min = shrink ? (s->size < floor ? s->size : floor) : s->size;
    *max = grow ? align_down(s->size + after) : s->size;
}

static char *op_text(const dk_op *op, const dk_seg *s, const dk_disk *d)
{
    char *size = size_text(op->size), *from = s ? size_text(s->size) : NULL, *label = empty(op->label) ? fr_strdup("") : fr_format(" \"%s\"", op->label);
    char *t;
    const char *what = s ? short_device(s) : "";
    switch (op->type) {
    case OP_IMAGE: {
        char *whole = size_text(d->size);
        t = fr_format("Write %s (%s) to %s, erasing all %s", base_name(op->file), size, d->device, whole);
        free(whole);
        break;
    }
    case OP_DELETE: t = fr_format("Delete %s (%s, %s)", what, empty(s->fs_type) ? "no filesystem" : s->fs_type, from); break;
    case OP_CREATE: t = fr_format("Create %s %s partition%s", size, empty(op->fs_type) ? "unformatted" : op->fs_type, label); break;
    case OP_FORMAT: t = fr_format("Format %s as %s%s", what, empty(op->fs_type) ? "empty" : op->fs_type, label); break;
    case OP_LABEL: t = fr_format("Label %s%s", what, label); break;
    default: t = fr_format("Resize %s from %s to %s", what, from, size); break;
    }
    free(size), free(from), free(label);
    return t;
}

static void set_text(dk_op *op, char *text)
{
    free(op->text);
    op->text = text;
}

static int apply_image(dk_disk *disk, dk_op *op, const char **error)
{
    if (disk->root_disk) return *error = "The running system lives on this disk.", 0;
    if (!disk->removable && !eq(disk->connection, "usb")) return *error = "Images go only onto removable or USB disks.", 0;
    for (int i = 0; i < disk->segs.n; i++)
        if (disk->segs.v[i].mounts.n) return *error = "A partition on this disk is mounted: unmount it first.", 0;
    if (op->size > disk->size) {
        static char buf[128];
        char a[32], b[32];
        dk_size_text(op->size, a, sizeof a), dk_size_text(disk->size, b, sizeof b);
        snprintf(buf, sizeof buf, "The image (%s) is larger than the disk (%s).", a, b);
        return *error = buf, 0;
    }
    set_text(op, op_text(op, NULL, disk));
    for (int i = 0; i < disk->segs.n; i++) dk_seg_free(&disk->segs.v[i]);
    disk->segs.n = 0;
    dk_seg img = {.key = fr_strdup("image"), .kind = SEG_IMAGE, .device = fr_strdup(disk->device), .label = fr_strdup(base_name(op->file)),
                  .size = op->size, .fs_type = fr_strdup(""), .group = fr_strdup("Image"), .group_kind = GROUP_NEW, .change = CHANGE_IMAGE};
    FR_PUSH(disk->segs, img);
    if (disk->size - op->size >= DK_MIN_FREE) {
        dk_seg rest = dk_free_seg(op->size, disk->size - op->size);
        rest.change = CHANGE_IMAGE;
        rest.was = fr_strdup("unused after the image");
        FR_PUSH(disk->segs, rest);
    }
    disk->imaged = 1;
    return 1;
}

static int apply_create(dk_disk *disk, dk_op *op, const char **error)
{
    dk_segs *segs = &disk->segs;
    int i = 0;
    for (; i < segs->n; i++) {
        const dk_seg *f = &segs->v[i];
        if (f->kind == SEG_FREE && op->offset >= f->offset && op->offset + op->size <= f->offset + f->size) break;
    }
    if (i == segs->n || op->size < DK_MIB) return *error = "The new partition must fit inside free space.", 0;
    uint64_t fo = segs->v[i].offset, fend = fo + segs->v[i].size;
    set_text(op, op_text(op, &segs->v[i], disk));
    const char *fs = op->fs_type ? op->fs_type : "";
    dk_seg part = {.key = fr_strdup(op->target), .kind = SEG_PARTITION, .device = fr_strdup("new partition"), .offset = op->offset,
                   .size = op->size, .fs_type = fr_strdup(fs), .label = fr_strdup(op->label ? op->label : ""),
                   .has_fs = *fs && strcmp(fs, "swap"), .resize_modes = DK_OFFLINE_SHRINK | DK_OFFLINE_GROW,
                   .group = fr_strdup("New"), .group_kind = GROUP_NEW, .change = CHANGE_CREATE};
    segs_remove(segs, i);
    int at = i;
    if (op->offset > fo) segs_insert(segs, at++, dk_free_seg(fo, op->offset - fo));
    segs_insert(segs, at++, part);
    if (fend > op->offset + op->size) segs_insert(segs, at, dk_free_seg(op->offset + op->size, fend - op->offset - op->size));
    tidy(segs);
    return 1;
}

static int apply_op(dk_disk *disk, dk_op *op, const char **error)
{
    if (disk->imaged && op->type != OP_IMAGE) return *error = "This disk is being overwritten by an image.", 0;
    if (op->type == OP_IMAGE) return apply_image(disk, op, error);
    if (op->type == OP_CREATE) return apply_create(disk, op, error);
    dk_segs *segs = &disk->segs;
    int i = dk_seg_index(segs, op->target);
    if (i < 0) return *error = "That partition no longer exists.", 0;
    dk_seg *s = &segs->v[i];
    const char *why = reason(s, op->type);
    if (why) return *error = why, 0;
    set_text(op, op_text(op, s, disk));
    op->from = s->size;
    if (op->type == OP_DELETE) {
        dk_seg f = dk_free_seg(s->offset, s->size);
        f.change = CHANGE_DELETE;
        f.was = fr_strdup(short_device(s));
        dk_seg_free(s);
        *s = f;
    } else if (op->type == OP_FORMAT) {
        const char *fs = op->fs_type ? op->fs_type : "";
        s->has_used = 0, s->used = s->used_total = 0;
        free(s->uuid), s->uuid = fr_strdup("");
        free(s->fs_type), s->fs_type = fr_strdup(fs);
        free(s->label), s->label = fr_strdup(op->label ? op->label : "");
        s->has_fs = *fs && strcmp(fs, "swap");
        free(s->group), s->group = fr_strdup("New");
        s->group_kind = GROUP_NEW, s->inferred = 0, s->change = CHANGE_FORMAT;
    } else if (op->type == OP_LABEL) {
        free(s->label), s->label = fr_strdup(op->label ? op->label : "");
        if (!s->change) s->change = CHANGE_LABEL;
    } else {
        uint64_t min, max;
        limits(segs, i, &min, &max);
        if (op->size < min || op->size > max) {
            static char buf[128];
            char a[32], b[32];
            dk_size_text(min, a, sizeof a), dk_size_text(max, b, sizeof b);
            snprintf(buf, sizeof buf, "Size must be between %s and %s.", a, b);
            return *error = buf, 0;
        }
        uint64_t offset = s->offset, free_end = offset + s->size;
        if (i + 1 < segs->n && segs->v[i + 1].kind == SEG_FREE) {
            free_end = segs->v[i + 1].offset + segs->v[i + 1].size;
            segs_remove(segs, i + 1);
            s = &segs->v[i];
        }
        s->size = op->size;
        if (!s->change) s->change = CHANGE_RESIZE;
        if (free_end > offset + op->size) segs_insert(segs, i + 1, dk_free_seg(offset + op->size, free_end - offset - op->size));
    }
    tidy(segs);
    return 1;
}

/* runs ops over the base into disks; lenient drops what no longer applies (dropped gets their texts), else the first
 * refusal stops it */
static int replay(dk_plan *p, dk_ops *ops, dk_disks *disks, const char **error, char *dropped, size_t dropped_len, int lenient)
{
    dk_disks_free(disks);
    *disks = dk_disks_copy(&p->base);
    if (dropped) *dropped = 0;
    for (int k = 0; k < ops->n;) {
        int d = dk_disk_index(disks, ops->v[k].disk);
        const char *why = "That disk is gone.";
        if (d >= 0) {
            dk_disk copy = dk_disk_copy(&disks->v[d]);
            if (apply_op(&copy, &ops->v[k], &why)) {
                dk_disk_free(&disks->v[d]);
                disks->v[d] = copy;
                k++;
                continue;
            }
            dk_disk_free(&copy);
        }
        if (!lenient) {
            if (error) *error = why;
            return 0;
        }
        if (dropped) {
            size_t n = strlen(dropped);
            snprintf(dropped + n, dropped_len - n, "%s%s: %s", n ? "; " : "", ops->v[k].text ? ops->v[k].text : "a step", why);
        }
        ops_remove(ops, k);
    }
    return 1;
}

static void project(dk_plan *p)
{
    if (p->running) {
        dk_ops rest = ops_copy(&p->ops, p->done);
        replay(p, &rest, &p->disks, NULL, NULL, 0, 1);
        ops_free(&rest);
        return;
    }
    char dropped[400];
    int before = p->ops.n;
    replay(p, &p->ops, &p->disks, NULL, dropped, sizeof dropped, 1);
    if (p->ops.n != before) snprintf(p->error, sizeof p->error, "Dropped, the disk changed: %s", dropped);
}

void dk_plan_free(dk_plan *p)
{
    dk_disks_free(&p->base);
    dk_disks_free(&p->disks);
    ops_free(&p->ops);
    *p = (dk_plan){0};
}

void dk_plan_set_base(dk_plan *p, dk_disks *base)
{
    dk_disks_free(&p->base);
    p->base = *base;
    *base = (dk_disks){0};
    project(p);
}

static const char *refuse(dk_plan *p, const char *why)
{
    if (why != p->why) snprintf(p->why, sizeof p->why, "%s", why);
    return p->why;
}

/* takes op */
static const char *stage(dk_plan *p, dk_op op)
{
    if (p->running) {
        op_free(&op);
        return refuse(p, "Wait for the running changes to finish.");
    }
    dk_ops ops = ops_copy(&p->ops, 0);
    int existing = -1;
    for (int k = 0; k < ops.n; k++) {
        const dk_op *o = &ops.v[k];
        if (is_new(op.target) ? eq(o->target, op.target) && o->type == OP_CREATE : eq(o->target, op.target) && o->type == op.type)
            existing = k;
    }
    if (op.type == OP_CREATE) FR_PUSH(ops, op);
    else if (is_new(op.target)) {
        if (existing < 0) {
            op_free(&op);
            ops_free(&ops);
            return refuse(p, "That partition is not staged any more.");
        }
        dk_op *c = &ops.v[existing];
        if (op.type == OP_DELETE) ops_remove(&ops, existing);
        else if (op.type == OP_RESIZE) c->size = op.size;
        else if (op.type == OP_FORMAT) {
            free(c->fs_type), c->fs_type = dup_or_null(op.fs_type);
            free(c->label), c->label = dup_or_null(op.label);
        } else if (op.type == OP_LABEL) free(c->label), c->label = dup_or_null(op.label);
        op_free(&op);
    } else if (existing >= 0 && op.type != OP_DELETE) {
        op_free(&ops.v[existing]);
        ops.v[existing] = op;
    } else FR_PUSH(ops, op);

    dk_disks disks = {0};
    const char *error = NULL;
    if (!replay(p, &ops, &disks, &error, NULL, 0, 0)) {
        snprintf(p->why, sizeof p->why, "%s", error);
        dk_disks_free(&disks);
        ops_free(&ops);
        return p->why;
    }
    ops_free(&p->ops);
    p->ops = ops;
    dk_disks_free(&p->disks);
    p->disks = disks;
    p->error[0] = 0;
    return NULL;
}

static dk_op new_op(int type, const char *disk, const char *target)
{
    return (dk_op){.type = type, .disk = fr_strdup(disk), .target = fr_strdup(target)};
}

const char *dk_plan_stage_delete(dk_plan *p, const char *disk, const char *key) { return stage(p, new_op(OP_DELETE, disk, key)); }

const char *dk_plan_stage_create(dk_plan *p, const char *disk, uint64_t offset, uint64_t size, const char *fs, const char *label)
{
    uint64_t start = align_up(offset), end = align_down(offset + size);
    if (end < start + DK_MIB) return refuse(p, "Too small: partitions are at least 1 MiB.");
    char target[32];
    snprintf(target, sizeof target, "new:%d", ++p->next_id);
    dk_op op = new_op(OP_CREATE, disk, target);
    op.offset = start, op.size = end - start;
    op.fs_type = fr_strdup(fs ? fs : ""), op.label = fr_strdup(label ? label : "");
    return stage(p, op);
}

const char *dk_plan_stage_format(dk_plan *p, const char *disk, const char *key, const char *fs, const char *label)
{
    dk_op op = new_op(OP_FORMAT, disk, key);
    op.fs_type = fr_strdup(fs ? fs : ""), op.label = fr_strdup(label ? label : "");
    return stage(p, op);
}

const char *dk_plan_stage_label(dk_plan *p, const char *disk, const char *key, const char *label)
{
    dk_op op = new_op(OP_LABEL, disk, key);
    op.label = fr_strdup(label ? label : "");
    return stage(p, op);
}

const char *dk_plan_stage_resize(dk_plan *p, const char *disk, const char *key, uint64_t size)
{
    dk_op op = new_op(OP_RESIZE, disk, key);
    op.size = align_down(size);
    return stage(p, op);
}

const char *dk_plan_stage_image(dk_plan *p, const char *disk, const char *file)
{
    char why[256];
    if (dk_image_check(file, why, sizeof why)) return refuse(p, why);
    struct stat st;
    if (stat(file, &st)) return refuse(p, "Cannot read the image.");
    dk_op op = new_op(OP_IMAGE, disk, disk);
    op.file = fr_strdup(file);
    op.size = (uint64_t)st.st_size;
    return stage(p, op);
}

const char *dk_plan_blocked(dk_plan *p, const char *disk, const char *key, int action)
{
    int d = dk_disk_index(&p->disks, disk);
    if (d < 0) return refuse(p, "Nothing selected.");
    if (p->disks.v[d].imaged) return refuse(p, "This disk is being overwritten by an image.");
    const dk_segs *segs = &p->disks.v[d].segs;
    int i = dk_seg_index(segs, key);
    if (i < 0) return refuse(p, "Nothing selected.");
    const dk_seg *s = &segs->v[i];
    if (is_new(key)) return action == OP_LABEL && !s->has_fs ? refuse(p, "No filesystem to label.") : NULL;
    const char *why = reason(s, action);
    if (why || action != OP_RESIZE) return why ? refuse(p, why) : NULL;
    uint64_t min, max;
    limits(segs, i, &min, &max);
    if (max - min < DK_MIN_FREE)
        return refuse(p, s->mounts.n ? "Mounted, so it can only grow, and there is no free space after it."
                                     : "No room: it cannot shrink, and there is no free space after it.");
    return NULL;
}

int dk_plan_limits(const dk_plan *p, const char *disk, const char *key, uint64_t *min, uint64_t *max)
{
    int d = dk_disk_index(&p->disks, disk);
    int i = d < 0 ? -1 : dk_seg_index(&p->disks.v[d].segs, key);
    if (i < 0) return 0;
    limits(&p->disks.v[d].segs, i, min, max);
    return 1;
}

void dk_plan_remove(dk_plan *p, int index)
{
    if (p->running || index < 0 || index >= p->ops.n) return;
    ops_remove(&p->ops, index);
    int kept = p->ops.n;
    char dropped[400];
    replay(p, &p->ops, &p->disks, NULL, dropped, sizeof dropped, 1);
    if (p->ops.n < kept) snprintf(p->error, sizeof p->error, "Also dropped, it depended on that step: %s", dropped);
    else p->error[0] = 0;
}

void dk_plan_clear(dk_plan *p)
{
    if (p->running) return;
    ops_free(&p->ops);
    p->error[0] = 0;
    project(p);
}

void dk_plan_confirm_words(const dk_plan *p, char *buf, size_t len)
{
    size_t n = 0;
    buf[0] = 0;
    for (int k = 0; k < p->ops.n; k++) {
        int d = dk_disk_index(&p->base, p->ops.v[k].disk);
        const char *dev = d < 0 ? "" : base_name(p->base.v[d].device);
        if (!*dev) continue;
        int seen = 0;
        for (int j = 0; j < k && !seen; j++) {
            int e = dk_disk_index(&p->base, p->ops.v[j].disk);
            seen = e >= 0 && eq(base_name(p->base.v[e].device), dev);
        }
        if (!seen && n < len) n += (size_t)snprintf(buf + n, len - n, "%s%s", n ? " " : "", dev);
    }
}

/* ---- running ---- */

const char *dk_call_method(int method)
{
    switch (method) {
    case CALL_DELETE: return "Delete";
    case CALL_CREATE: return "CreatePartition";
    case CALL_CREATE_FORMAT: return "CreatePartitionAndFormat";
    case CALL_FORMAT: return "Format";
    case CALL_SET_LABEL: return "SetLabel";
    case CALL_PART_RESIZE:
    case CALL_FS_RESIZE: return "Resize";
    default: return "OpenDevice";
    }
}

static const char *partition_type(const char *fs, const char *table)
{
    int gpt = eq(table, "gpt");
    if (eq(fs, "swap")) return gpt ? "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f" : "0x82";
    if (eq(fs, "ntfs") || eq(fs, "exfat")) return gpt ? "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7" : "0x07";
    if (eq(fs, "vfat")) return gpt ? "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7" : "0x0c";
    return gpt ? "0fc63daf-8483-4772-8e79-3d69d8477de4" : "0x83";
}

static dk_call call(int method, const char *object)
{
    return (dk_call){.method = method, .object = fr_strdup(object ? object : "")};
}

/* Resizes order the filesystem and the partition so the filesystem always fits: shrink the filesystem first, grow
 * the partition first (Filesystem.Resize(0) fills it). */
static dk_step op_step(const dk_op *op, const dk_disk *disk)
{
    dk_step s = {0};
    const char *fs = op->fs_type ? op->fs_type : "";
    dk_call c;
    switch (op->type) {
    case OP_DELETE: FR_PUSH(s, call(CALL_DELETE, op->target)); break;
    case OP_CREATE:
        c = call(*fs ? CALL_CREATE_FORMAT : CALL_CREATE, disk ? disk->block : NULL);
        c.offset = op->offset, c.size = op->size;
        c.ptype = fr_strdup(partition_type(fs, disk ? disk->table_type : NULL));
        c.fs = fr_strdup(fs), c.label = fr_strdup(op->label ? op->label : "");
        FR_PUSH(s, c);
        break;
    case OP_FORMAT:
        c = call(CALL_FORMAT, op->target);
        c.fs = fr_strdup(*fs ? fs : "empty"), c.label = fr_strdup(op->label ? op->label : "");
        FR_PUSH(s, c);
        break;
    case OP_LABEL:
        c = call(CALL_SET_LABEL, op->target);
        c.label = fr_strdup(op->label ? op->label : "");
        FR_PUSH(s, c);
        break;
    case OP_RESIZE: {
        int grow = op->size > op->from, i = disk ? dk_seg_index(&disk->segs, op->target) : -1;
        int has_fs = i >= 0 && disk->segs.v[i].has_fs;
        dk_call part = call(CALL_PART_RESIZE, op->target), fsc = call(CALL_FS_RESIZE, op->target);
        part.size = op->size, fsc.size = grow ? 0 : op->size;
        if (!has_fs) {
            FR_PUSH(s, part);
            free(fsc.object);
        } else if (grow) {
            FR_PUSH(s, part);
            FR_PUSH(s, fsc);
        } else {
            FR_PUSH(s, fsc);
            FR_PUSH(s, part);
        }
        break;
    }
    default:
        c = call(CALL_IMAGE, disk ? disk->block : NULL);
        c.size = op->size, c.file = fr_strdup(op->file);
        FR_PUSH(s, c);
        break;
    }
    return s;
}

int dk_plan_begin(dk_plan *p, dk_steps *out)
{
    if (p->running || !p->ops.n) return 0;
    for (int k = 0; k < p->ops.n; k++) {
        int d = dk_disk_index(&p->base, p->ops.v[k].disk);
        FR_PUSH(*out, op_step(&p->ops.v[k], d < 0 ? NULL : &p->base.v[d]));
    }
    p->running = 1;
    p->done = 0;
    p->total = p->ops.n;
    p->error[0] = 0;
    return 1;
}

void dk_plan_step_done(dk_plan *p)
{
    if (!p->running) return;
    p->done++;
    project(p);
}

void dk_plan_finish(dk_plan *p, const char *error)
{
    if (!p->running) return;
    if (error) {
        int at = p->done < p->ops.n ? p->done : p->ops.n - 1;
        snprintf(p->error, sizeof p->error, "%s failed: %s", at >= 0 && p->ops.v[at].text ? p->ops.v[at].text : "A step", error);
        dk_ops rest = ops_copy(&p->ops, at < 0 ? 0 : at);
        ops_free(&p->ops);
        p->ops = rest;
    } else ops_free(&p->ops);
    p->running = 0;
    project(p);
}

void dk_steps_free(dk_steps *s)
{
    for (int i = 0; i < s->n; i++) {
        for (int k = 0; k < s->v[i].n; k++) {
            dk_call *c = &s->v[i].v[k];
            free(c->object), free(c->ptype), free(c->fs), free(c->label), free(c->file);
        }
        free(s->v[i].v);
    }
    free(s->v);
    *s = (dk_steps){0};
}

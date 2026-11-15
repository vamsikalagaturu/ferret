/* The disk manager: every disk stacked as a bar (its length the disk's size against the largest, at least 35%),
 * partitions and free space in disk order coloured by the OS they belong to, with brackets naming the OS under the
 * bar; an inspector beside it with the selected partition's details, actions and forms; the queue of staged changes
 * under the map, applied after a dialog that wants the disks' names typed. Where the platform has no disk manager
 * (Android) the view is the storage overview of disks_usage.c instead. */
#define _GNU_SOURCE
#include "disks_view.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_DISKS 32

static const char *const fs_all[] = {"ext4", "btrfs", "xfs", "vfat", "exfat", "ntfs", "swap"};
enum { A_MOUNT = 100, A_UNMOUNT, A_USAGE };

typedef struct {
    int id;
    const char *key, *text, *icon;
    char why[200];
    int danger;
} action;

static struct {
    int open, loaded, available;
    unsigned serial;
    dk_plan plan;
    fr_strs formats;
    char *disk, *key; /* the selection */
    int mode;         /* the inspector's form: OP_CREATE, OP_FORMAT, OP_LABEL, OP_RESIZE */
    uint64_t create_offset, create_size;
    fr_text size_f, label_f;
    char fs[16], form_error[256];
    int confirm, image;
    fr_text confirm_f, image_f;
    char *image_disk;
    fr_scroll map_scroll, side_scroll, queue_scroll;
    struct {
        int kind; /* 1 resizing at the right edge, 2 marking free space */
        int disk, seg;
        float start, x0, x1, seg_x, seg_w;
        double per_px;
        uint64_t size, min, max;
    } drag;
    fr_dm_run run;
    unsigned menu;
    int pending;
    float disk_top[MAX_DISKS], disk_bottom[MAX_DISKS];
    Rectangle sel_rect;
} V;

Color dview_group_color(int kind)
{
    switch (kind) {
    case GROUP_RUNNING: return theme.success;
    case GROUP_WINDOWS: return theme.document;
    case GROUP_OS: return theme.image;
    case GROUP_NEW: return theme.accent;
    default: return theme.warning;
    }
}

static const char *const change_names[] = {"", "create", "delete", "format", "label", "resize", "image"};

/* ---- the model ---- */

static void reload(void)
{
    V.loaded = 1;
    V.serial = plat_dm_serial();
    dk_tree t = {0};
    dk_disks d = {0};
    V.available = !plat_dm_tree(&t);
    if (V.available) {
        dk_env env = {0};
        dk_env_read(&env);
        dk_build(&t, &env, &d);
        dk_env_free(&env);
        fr_strs_free(&V.formats);
        V.formats = fr_strs_copy(&t.formats);
    }
    dk_tree_free(&t);
    dk_plan_set_base(&V.plan, &d);
    ui.dirty = 1;
}

static dk_disk *cur_disk(void)
{
    int i = dk_disk_index(&V.plan.disks, V.disk);
    return i < 0 ? NULL : &V.plan.disks.v[i];
}

static dk_seg *cur_seg(void)
{
    dk_disk *d = cur_disk();
    int i = d ? dk_seg_index(&d->segs, V.key) : -1;
    return i < 0 ? NULL : &d->segs.v[i];
}

static void select_seg(const char *disk, const char *key)
{
    char *d = disk ? fr_strdup(disk) : NULL, *k = key ? fr_strdup(key) : NULL;
    free(V.disk), free(V.key);
    V.disk = d, V.key = k;
    V.mode = 0;
    V.form_error[0] = 0;
    ui.dirty = 1;
}

static int ensure_selection(void)
{
    if (cur_disk()) {
        if (!cur_seg() && cur_disk()->segs.n) select_seg(V.disk, cur_disk()->segs.v[0].key);
        return 1;
    }
    if (!V.plan.disks.n) return 0;
    const dk_disk *d = &V.plan.disks.v[0];
    select_seg(d->path, d->segs.n ? d->segs.v[0].key : NULL);
    return 0;
}

char *dview_mount_of(const char *key)
{
    if (!plat_dm_supported()) return plat_volume_path(key);
    for (int i = 0; i < V.plan.base.n; i++)
        for (int k = 0; k < V.plan.base.v[i].segs.n; k++) {
            const dk_seg *s = &V.plan.base.v[i].segs.v[k];
            if (s->path && key && !strcmp(s->path, key)) return s->mounts.n ? fr_strdup(s->mounts.v[0]) : NULL;
        }
    return NULL;
}

/* ---- actions ---- */

static void why_of(action *a, const char *why) { snprintf(a->why, sizeof a->why, "%s", why ? why : ""); }

static int actions_for(const dk_disk *d, const dk_seg *s, action *out)
{
    int n = 0;
    if (!d || !s) return 0;
    if (s->kind == SEG_FREE) {
        out[n] = (action){OP_CREATE, "n", "New partition...", "plus", "", 0};
        why_of(&out[n++], dk_plan_blocked(&V.plan, d->path, s->key, OP_CREATE));
        return n;
    }
    int real = strncmp(s->key, "new:", 4) && !s->change && s->kind != SEG_IMAGE;
    if (real && s->has_fs) {
        int mounted = s->mounts.n > 0;
        out[n] = (action){mounted ? A_UNMOUNT : A_MOUNT, "m", mounted ? "Unmount" : "Mount", mounted ? "eject" : "hard-drive", "", 0};
        why_of(&out[n++], mounted && s->system_mount ? "In use by the running system." : s->encrypted ? "Encrypted: read-only here." : NULL);
        out[n] = (action){A_USAGE, "o", "Look inside", "search", "", 0};
        why_of(&out[n++], s->encrypted ? "Encrypted: ferret cannot unlock it." : NULL);
    }
    static const struct {
        int id;
        const char *key, *text, *icon;
        int danger;
    } staged[] = {{OP_RESIZE, "r", "Resize...", "ruler", 0},
                  {OP_FORMAT, "f", "Format...", "refresh-cw", 0},
                  {OP_LABEL, "t", "Set label...", "pencil", 0},
                  {OP_DELETE, "dd", "Delete", "trash-2", 1}};
    for (int i = 0; i < 4; i++) {
        out[n] = (action){staged[i].id, staged[i].key, staged[i].text, staged[i].icon, "", staged[i].danger};
        why_of(&out[n++], dk_plan_blocked(&V.plan, d->path, s->key, staged[i].id));
    }
    return n;
}

static void size_input(fr_text *t, uint64_t bytes)
{
    char b[48];
    snprintf(b, sizeof b, "%.2f GiB", floor((double)bytes / (1024.0 * 1024 * 1024) * 100) / 100);
    text_set(t, b);
}

static unsigned size_id(void) { return ui_id("dview-size", 0); }
static unsigned label_id(void) { return ui_id("dview-label", 0); }

static int fs_choices(const char **out, int with_none)
{
    int n = 0;
    for (size_t i = 0; i < sizeof fs_all / sizeof *fs_all; i++)
        for (int k = 0; k < V.formats.n; k++)
            if (!strcmp(V.formats.v[k], fs_all[i])) {
                out[n++] = fs_all[i];
                break;
            }
    if (with_none) out[n++] = "none";
    return n;
}

static void open_form(int mode)
{
    dk_seg *s = cur_seg();
    if (!s) return;
    V.mode = mode;
    V.form_error[0] = 0;
    text_set(&V.label_f, mode == OP_CREATE ? "" : s->label ? s->label : "");
    const char *choices[8];
    int n = fs_choices(choices, 0), keep = 0;
    for (int i = 0; i < n; i++) keep |= s->fs_type && !strcmp(s->fs_type, choices[i]);
    snprintf(V.fs, sizeof V.fs, "%s", keep ? s->fs_type : "ext4");
    size_input(&V.size_f, mode == OP_CREATE ? V.create_size : s->size);
    ui.focus = mode == OP_RESIZE || mode == OP_CREATE ? size_id() : label_id();
    if (ui.focus == size_id()) text_select(&V.size_f, 0, V.size_f.len);
    ui.dirty = 1;
}

static void close_form(void)
{
    V.mode = 0;
    if (ui.focus == size_id() || ui.focus == label_id()) ui.focus = 0;
    ui.dirty = 1;
}

static void cycle_fs(int step)
{
    if (V.mode != OP_FORMAT && V.mode != OP_CREATE) return;
    const char *choices[8];
    int n = fs_choices(choices, V.mode == OP_CREATE), at = 0;
    if (!n) return;
    for (int i = 0; i < n; i++)
        if (!strcmp(choices[i], V.fs)) at = i;
    snprintf(V.fs, sizeof V.fs, "%s", choices[((at + step) % n + n) % n]);
    ui.dirty = 1;
}

static void submit(void)
{
    dk_disk *d = cur_disk();
    dk_seg *s = cur_seg();
    if (!d || !s) return;
    char *disk = fr_strdup(d->path), *key = fr_strdup(s->key);
    const char *err = NULL;
    if (V.mode == OP_FORMAT) err = dk_plan_stage_format(&V.plan, disk, key, V.fs, V.label_f.buf);
    else if (V.mode == OP_LABEL) err = dk_plan_stage_label(&V.plan, disk, key, V.label_f.buf);
    else {
        int64_t size = dk_parse_size(V.size_f.buf);
        if (size < 0) err = "Size not understood: try 200 GiB or 512 MiB.";
        else if (V.mode == OP_RESIZE) err = dk_plan_stage_resize(&V.plan, disk, key, (uint64_t)size);
        else {
            uint64_t room = s->offset + s->size - V.create_offset;
            err = dk_plan_stage_create(&V.plan, disk, V.create_offset, (uint64_t)size < room ? (uint64_t)size : room,
                                       strcmp(V.fs, "none") ? V.fs : "", V.label_f.buf);
        }
    }
    snprintf(V.form_error, sizeof V.form_error, "%s", err ? err : "");
    if (!err) close_form();
    free(disk), free(key);
}

static void stage_result(const char *err)
{
    if (err) ui_toast("%s", err);
}

static void image_start(const dk_disk *d)
{
    free(V.image_disk);
    V.image_disk = fr_strdup(d->path);
    fr_item *it = browser_item(br.cursor);
    const char *ext = it && !it->e.is_dir ? fr_ext(it->e.name) : "";
    char *uri = it && (!strcasecmp(ext, "iso") || !strcasecmp(ext, "img")) && br.uri ? fr_uri_join(br.uri, it->e.name) : NULL;
    const char *local = uri ? fr_uri_local(uri) : br.uri ? fr_uri_local(br.uri) : NULL;
    char *text = uri && local ? fr_strdup(local) : local ? fr_format("%s/", strcmp(local, "/") ? local : "") : fr_strdup("");
    text_set(&V.image_f, text);
    free(text), free(uri);
    V.image = 1;
}

static void trigger(int id)
{
    dk_disk *d = cur_disk();
    dk_seg *s = cur_seg();
    if (!d || !s) return;
    if (id == A_MOUNT) plat_disk_request(DISK_MOUNT, s->path);
    else if (id == A_UNMOUNT) plat_disk_request(DISK_UNMOUNT, s->path);
    else if (id == A_USAGE) usage_open(s->path, s->mounts.n ? s->mounts.v[0] : NULL, dk_seg_name(s));
    else if (id == OP_DELETE) {
        char *disk = fr_strdup(d->path), *key = fr_strdup(s->key);
        stage_result(dk_plan_stage_delete(&V.plan, disk, key));
        free(disk), free(key);
    } else {
        if (id == OP_CREATE) V.create_offset = s->offset, V.create_size = s->size;
        open_form(id);
    }
}

static void run_action(int id)
{
    if (!ensure_selection()) return;
    action a[8];
    int n = actions_for(cur_disk(), cur_seg(), a);
    for (int i = 0; i < n; i++)
        if (a[i].id == id || (id == A_MOUNT && a[i].id == A_UNMOUNT)) {
            if (a[i].why[0]) ui_toast("%s", a[i].why);
            else trigger(a[i].id);
            return;
        }
}

static void disk_key(int c)
{
    dk_disk *d = cur_disk();
    if (!d || d->root_disk) return;
    if (c == 'w' && (d->removable || !strcmp(d->connection, "usb"))) image_start(d);
    else if (c == 'E' && d->ejectable) plat_disk_request(DISK_EJECT, d->block);
    else if (c == 'P' && d->can_power_off) plat_disk_request(DISK_POWER_OFF, d->block);
}

static void apply(void)
{
    dk_steps steps = {0};
    if (dk_plan_begin(&V.plan, &steps)) plat_dm_apply(&steps);
    dk_steps_free(&steps);
}

/* ---- moving ---- */

static void move_segment(int step)
{
    if (!ensure_selection()) return;
    dk_disk *d = cur_disk();
    if (!d->segs.n) return;
    int i = dk_seg_index(&d->segs, V.key);
    long to = (long)(i < 0 ? 0 : i) + step;
    to = to < 0 ? 0 : to >= d->segs.n ? d->segs.n - 1 : to;
    select_seg(d->path, d->segs.v[to].key);
}

static void move_disk(int step)
{
    if (!ensure_selection()) return;
    int di = dk_disk_index(&V.plan.disks, V.disk), si = dk_seg_index(&cur_disk()->segs, V.key);
    long to = (long)di + step;
    to = to < 0 ? 0 : to >= V.plan.disks.n ? V.plan.disks.n - 1 : to;
    const dk_disk *d = &V.plan.disks.v[to];
    if (si < 0) si = 0;
    select_seg(d->path, d->segs.n ? d->segs.v[si < d->segs.n ? si : d->segs.n - 1].key : NULL);
    if (to < MAX_DISKS) ui_scroll_to(&V.map_scroll, V.disk_top[to], V.disk_bottom[to]);
}

static void open_menu_at_selection(void)
{
    if (!ensure_selection() || !cur_seg()) return;
    Rectangle r = V.sel_rect;
    ui_menu_open(V.menu, (Rectangle){r.x + r.width / 2, r.y + r.height / 2, 0, 0});
}

/* ---- drawing pieces ---- */

static float chip(float x, float y, float h, const char *text, Color tint)
{
    float w = ui_text_width(text, 12, FONT_SEMIBOLD) + DP(14);
    Rectangle r = {x, y + (h - DP(20)) / 2, w, DP(20)};
    ui_round(r, 6, ui_alpha(tint, 0.18f));
    ui_text_box(text, r, 12, FONT_SEMIBOLD, tint, ALIGN_CENTER, 0);
    return w;
}

static int small_button(const char *id, float x, float y, const char *label, const char *hint, float *w_out)
{
    float lw = ui_text_width(label, 13, FONT_SEMIBOLD), hw = hint ? ui_text_width(hint, 12, FONT_REGULAR) + DP(10) : 0;
    float w = lw + hw + DP(20), h = DP(28);
    Rectangle r = {x, y, w, h};
    int state, clicked = ui_clickable(ui_id(id, 0), r, &state);
    ui_round(r, 7, state == 2 ? theme.pressed : state ? theme.hover : theme.surface);
    ui_round_line(r, 7, theme.border);
    ui_text_box(label, (Rectangle){x + DP(10), y, lw + DP(2), h}, 13, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    if (hint) ui_text_box(hint, (Rectangle){x + DP(10) + lw + DP(8), y, hw, h}, 12, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    *w_out = w;
    return clicked;
}

static float disk_height(const dk_disk *d)
{
    int buttons = !d->root_disk && (d->removable || !strcmp(d->connection, "usb") || d->ejectable || d->can_power_off);
    return DP(26) + (buttons ? DP(36) : 0) + DP(8) + DP(64) + DP(30);
}

static uint64_t size_at(const dk_disk *d, int di, int i)
{
    const dk_seg *s = &d->segs.v[i];
    if (V.drag.kind != 1 || V.drag.disk != di) return s->size;
    if (i == V.drag.seg) return V.drag.size;
    if (i == V.drag.seg + 1 && s->kind == SEG_FREE) {
        int64_t delta = (int64_t)V.drag.size - (int64_t)d->segs.v[V.drag.seg].size;
        return (int64_t)s->size - delta > 0 ? (uint64_t)((int64_t)s->size - delta) : 0;
    }
    return s->size;
}

static void draw_disk(int di, const dk_disk *d, float x, float y, float w, uint64_t largest, Rectangle clip)
{
    float lh = DP(26);
    float nw = fminf(ui_text_width(d->name, 15, FONT_SEMIBOLD) + DP(4), w * 0.5f);
    ui_text_box(d->name, (Rectangle){x, y, nw, lh}, 15, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    char size[32], info[256], table[16];
    dk_size_text(d->size, size, sizeof size);
    snprintf(table, sizeof table, "%s", d->table_type);
    for (char *c = table; *c; c++) *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    snprintf(info, sizeof info, "%s  |  %s%s%s%s%s", d->device, size, *table ? "  |  " : "", table, *d->connection ? "  |  " : "", d->connection);
    float iw = fminf(ui_text_width(info, 13, FONT_REGULAR) + DP(4), w - nw - DP(12));
    ui_text_box(info, (Rectangle){x + nw + DP(12), y, iw, lh}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    float cx = x + nw + DP(12) + iw + DP(10);
    if (d->root_disk) cx += chip(cx, y, lh, "running system", theme.success) + DP(6);
    if (d->removable) chip(cx, y, lh, "removable", theme.accent);
    y += lh;

    if (!d->root_disk && (d->removable || !strcmp(d->connection, "usb") || d->ejectable || d->can_power_off)) {
        float bx = x, bw;
        char id[64];
        snprintf(id, sizeof id, "dview-image-%d", di);
        if ((d->removable || !strcmp(d->connection, "usb")) && small_button(id, bx, y + DP(4), "Write bootable image...", "w", &bw))
            image_start(d);
        if (d->removable || !strcmp(d->connection, "usb")) bx += bw + DP(8);
        snprintf(id, sizeof id, "dview-eject-%d", di);
        if (d->ejectable && small_button(id, bx, y + DP(4), "Eject", "E", &bw)) plat_disk_request(DISK_EJECT, d->block);
        if (d->ejectable) bx += bw + DP(8);
        snprintf(id, sizeof id, "dview-poweroff-%d", di);
        if (d->can_power_off && small_button(id, bx, y + DP(4), "Power off", "P", &bw)) plat_disk_request(DISK_POWER_OFF, d->block);
        y += DP(36);
    }
    y += DP(8);

    /* inside the bar partitions are proportional, with a minimum width so a 16 MiB partition stays visible */
    int n = d->segs.n;
    float bar_w = w * fmaxf(0.35f, (float)((double)d->size / (double)largest)), gap = DP(3), min_w = DP(14), bh = DP(64);
    float spare = fmaxf(1, bar_w - n * min_w - (n > 1 ? n - 1 : 0) * gap);
    double total = 0;
    for (int i = 0; i < n; i++) total += (double)d->segs.v[i].size;
    float sx[64], sw[64];
    float at = x;
    for (int i = 0; i < n && i < 64; i++) {
        sx[i] = at;
        sw[i] = min_w + (total > 0 ? spare * (float)((double)size_at(d, di, i) / total) : 0);
        at += sw[i] + gap;
    }
    if (!n) ui_text_box("No partitions or filesystem", (Rectangle){x, y, w, bh}, 13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    for (int i = 0; i < n && i < 64; i++) {
        const dk_seg *s = &d->segs.v[i];
        Rectangle r = {sx[i], y, sw[i], bh};
        int free_seg = s->kind == SEG_FREE, freed = free_seg && s->change == CHANGE_DELETE;
        int sel = V.disk && V.key && !strcmp(V.disk, d->path) && !strcmp(V.key, s->key);
        Color tone = free_seg ? theme.border : dview_group_color(s->group_kind);
        if (sel) V.sel_rect = r;
        if (freed) ui_round(r, 8, ui_alpha(theme.danger, 0.14f));
        else if (!free_seg) ui_round(r, 8, ui_alpha(tone, 0.22f));
        if (s->has_used && s->used_total && !(V.drag.kind == 1 && V.drag.disk == di && V.drag.seg == i))
            ui_round((Rectangle){r.x + 1, r.y + 1, fmaxf(0, (r.width - 2) * (float)((double)s->used / (double)s->used_total)), r.height - 2}, 8,
                     ui_alpha(tone, 0.32f));
        if (V.drag.kind == 2 && V.drag.disk == di && V.drag.seg == i) {
            float a = fminf(V.drag.x0, V.drag.x1), b = fmaxf(V.drag.x0, V.drag.x1);
            Rectangle m = {r.x + a, r.y, b - a, r.height};
            ui_rect(m, ui_alpha(theme.accent, 0.3f));
            ui_round_line(m, 2, theme.accent);
        }
        ui_round_line(r, 8, sel ? theme.text : freed ? ui_alpha(theme.danger, 0.7f) : ui_alpha(tone, 0.85f));
        if (sel) ui_round_line((Rectangle){r.x + 1, r.y + 1, r.width - 2, r.height - 2}, 7, theme.text);
        if (r.width > DP(64)) {
            char name[200], line2[96];
            if (free_seg) snprintf(name, sizeof name, freed ? "freed: %s" : "%s", freed ? s->was : s->change == CHANGE_IMAGE ? "unused" : "Free");
            else snprintf(name, sizeof name, "%s", dk_seg_name(s));
            char sz[32];
            dk_size_text(size_at(d, di, i), sz, sizeof sz);
            snprintf(line2, sizeof line2, "%s%s%s", sz, free_seg || !s->fs_type || !*s->fs_type ? "" : "  ", free_seg || !s->fs_type ? "" : s->fs_type);
            float tx = r.x + DP(9), tw = r.width - DP(18), ty = r.y + (bh - ui_line_height(13) - ui_line_height(12)) / 2;
            ui_text_box(name, (Rectangle){tx, ty, tw, ui_line_height(13)}, 13, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
            ui_text_box(line2, (Rectangle){tx, ty + ui_line_height(13), tw, ui_line_height(12)}, 12, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
        }
        if (!free_seg && s->change && r.width > DP(90)) {
            const char *c = change_names[s->change];
            float cw = ui_text_width(c, 11, FONT_SEMIBOLD) + DP(10);
            Rectangle cr = {r.x + r.width - cw - DP(4), r.y + DP(4), cw, DP(18)};
            ui_round(cr, 5, ui_alpha(theme.accent, 0.25f));
            ui_text_box(c, cr, 11, FONT_SEMIBOLD, theme.accent, ALIGN_CENTER, 0);
        }

        /* drag the right edge to resize, drag across free space to make a partition, right-click for actions */
        int resizable = !free_seg && !dk_plan_blocked(&V.plan, d->path, s->key, OP_RESIZE);
        Rectangle handle = {r.x + r.width - DP(10), r.y, DP(10), r.height};
        if (resizable) {
            int lit = ui_hover(handle) || (V.drag.kind == 1 && V.drag.disk == di && V.drag.seg == i);
            ui_round((Rectangle){r.x + r.width - DP(6), r.y + r.height * 0.275f, DP(3), r.height * 0.45f}, 2, ui_alpha(theme.text, lit ? 1 : 0.35f));
        }
        Rectangle hit = GetCollisionRec(r, clip);
        if (!V.drag.kind && ui_live() && ui.pressed && CheckCollisionPointRec(ui.mouse, hit)) {
            char *dp = fr_strdup(d->path), *k = fr_strdup(s->key);
            select_seg(dp, k);
            free(dp), free(k);
            ui.active = ui_id("dview-bar", di * 64 + i);
            if (resizable && CheckCollisionPointRec(ui.mouse, handle) && dk_plan_limits(&V.plan, d->path, s->key, &V.drag.min, &V.drag.max)) {
                V.drag.kind = 1, V.drag.disk = di, V.drag.seg = i, V.drag.start = ui.mouse.x, V.drag.size = s->size;
                V.drag.per_px = total / spare;
            } else if (free_seg && s->change != CHANGE_IMAGE) {
                V.drag.kind = 2, V.drag.disk = di, V.drag.seg = i;
                V.drag.x0 = V.drag.x1 = ui.mouse.x - r.x, V.drag.seg_x = r.x, V.drag.seg_w = r.width;
            }
        }
        if (ui_live() && ui.rpressed && CheckCollisionPointRec(ui.mouse, hit)) {
            char *dp = fr_strdup(d->path), *k = fr_strdup(s->key);
            select_seg(dp, k);
            free(dp), free(k);
            ui_menu_open(V.menu, (Rectangle){ui.mouse.x, ui.mouse.y, 0, 0});
        }
    }
    y += bh;

    /* the OS groups: runs of neighbouring partitions of one OS under a bracket */
    for (int i = 0; i < n && i < 64;) {
        const dk_seg *s = &d->segs.v[i];
        if (s->kind == SEG_FREE) {
            i++;
            continue;
        }
        int end = i, inferred = s->inferred;
        while (end + 1 < n && end + 1 < 64 && d->segs.v[end + 1].kind != SEG_FREE && d->segs.v[end + 1].group && s->group &&
               !strcmp(d->segs.v[end + 1].group, s->group))
            end++, inferred &= d->segs.v[end].inferred;
        Color tone = dview_group_color(s->group_kind);
        float bx = sx[i], bw = sx[end] + sw[end] - sx[i], one = fmaxf(1, roundf(DP(1)));
        ui_rect((Rectangle){bx, y + DP(4), one, DP(6)}, tone);
        ui_rect((Rectangle){bx + bw - one, y + DP(4), one, DP(6)}, tone);
        ui_rect((Rectangle){bx, y + DP(10), bw, one}, tone);
        char label[200];
        snprintf(label, sizeof label, "%s%s", s->group ? s->group : "", inferred ? " (likely)" : "");
        if (ui_text_width(label, 12, FONT_REGULAR) <= bw - DP(4))
            ui_text_box(label, (Rectangle){bx + DP(2), y + DP(12), bw - DP(4), DP(16)}, 12, FONT_REGULAR, tone, ALIGN_LEFT, 0);
        i = end + 1;
    }
}

static void drag_update(void)
{
    if (!V.drag.kind) return;
    if (V.drag.disk >= V.plan.disks.n || V.drag.seg >= V.plan.disks.v[V.drag.disk].segs.n) {
        V.drag.kind = 0;
        return;
    }
    const dk_disk *d = &V.plan.disks.v[V.drag.disk];
    const dk_seg *s = &d->segs.v[V.drag.seg];
    if (V.drag.kind == 1) {
        double size = (double)s->size + (ui.mouse.x - V.drag.start) * V.drag.per_px;
        size = round(size / (double)DK_MIB) * (double)DK_MIB;
        size = fmax((double)V.drag.min, fmin((double)V.drag.max, size));
        V.drag.size = (uint64_t)size;
        ui.dirty = 1;
        if (!ui.down) {
            int changed = llabs((long long)V.drag.size - (long long)s->size) >= (long long)DK_MIB;
            uint64_t to = V.drag.size;
            char *disk = fr_strdup(d->path), *key = fr_strdup(s->key);
            V.drag.kind = 0;
            if (changed) stage_result(dk_plan_stage_resize(&V.plan, disk, key, to));
            free(disk), free(key);
        }
    } else {
        V.drag.x1 = fmaxf(0, fminf(V.drag.seg_w, ui.mouse.x - V.drag.seg_x));
        ui.dirty = 1;
        if (!ui.down) {
            V.drag.kind = 0;
            if (fabsf(V.drag.x1 - V.drag.x0) > DP(6)) {
                double a = fminf(V.drag.x0, V.drag.x1) / V.drag.seg_w, b = fmaxf(V.drag.x0, V.drag.x1) / V.drag.seg_w;
                V.create_offset = s->offset + (uint64_t)(a * (double)s->size);
                V.create_size = (uint64_t)((b - a) * (double)s->size);
                open_form(OP_CREATE);
            }
        }
    }
}

/* ---- the inspector ---- */

static float detail(float x, float y, float w, const char *k, const char *v)
{
    if (!v || !*v) return y;
    float kw = DP(96), lh = ui_line_height(13);
    ui_text_box(k, (Rectangle){x, y, kw, lh}, 13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    int lines = ui_text_wrapped(v, (Rectangle){x + kw + DP(8), y, w - kw - DP(8), 0}, 13, FONT_REGULAR, theme.text_dim, 1);
    return y + fmaxf(1, (float)lines) * lh + DP(5);
}

static float field_label(float x, float y, float w, const char *text)
{
    ui_text_box(text, (Rectangle){x, y, w, ui_line_height(12)}, 12, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    return y + ui_line_height(12) + DP(4);
}

static float draw_form(float x, float y, float w, const dk_disk *d, const dk_seg *s)
{
    static const char *const titles[] = {[OP_CREATE] = "New partition", [OP_FORMAT] = "Format", [OP_LABEL] = "Set label", [OP_RESIZE] = "Resize"};
    ui_text_box(titles[V.mode], (Rectangle){x, y, w, ui_line_height(15)}, 15, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    y += ui_line_height(15) + DP(10);
    if (V.mode == OP_FORMAT) {
        char *t = fr_format("Erases everything on %s.", s->device);
        y += ui_text_wrapped(t, (Rectangle){x, y, w, 0}, 13, FONT_REGULAR, theme.danger, 1) * ui_line_height(13) + DP(10);
        free(t);
    }
    if (V.mode == OP_RESIZE || V.mode == OP_CREATE) {
        y = field_label(x, y, w, "Size");
        int rc = ui_text_field(size_id(), &V.size_f, (Rectangle){x, y, w, DP(38)}, "e.g. 200 GiB");
        /* staging replaces the projected disks d and s point into */
        if (rc & TEXT_SUBMIT) return submit(), y;
        if (rc & TEXT_CANCEL) return close_form(), y;
        y += DP(38) + DP(6);
        char hint[200], a[32], b[32];
        uint64_t min = 0, max = 0;
        if (V.mode == OP_CREATE) {
            dk_size_text(s->offset + s->size - V.create_offset, a, sizeof a);
            snprintf(hint, sizeof hint, "Up to %s", a);
        } else {
            dk_plan_limits(&V.plan, d->path, s->key, &min, &max);
            dk_size_text(min, a, sizeof a), dk_size_text(max, b, sizeof b);
            snprintf(hint, sizeof hint, "Between %s and %s. The start stays put; only the end moves.", a, b);
        }
        y += ui_text_wrapped(hint, (Rectangle){x, y, w, 0}, 12, FONT_REGULAR, theme.text_faint, 1) * ui_line_height(12) + DP(10);
    }
    if (V.mode == OP_FORMAT || V.mode == OP_CREATE) {
        y = field_label(x, y, w, "Filesystem  (Tab / Shift+Tab)");
        const char *choices[8];
        int n = fs_choices(choices, V.mode == OP_CREATE);
        float cx = x, ch = DP(28);
        for (int i = 0; i < n; i++) {
            float cw = ui_text_width(choices[i], 13, FONT_SEMIBOLD) + DP(18);
            if (cx + cw > x + w) cx = x, y += ch + DP(6);
            Rectangle r = {cx, y, cw, ch};
            int on = !strcmp(choices[i], V.fs), state;
            if (ui_clickable(ui_id("dview-fs", i), r, &state)) snprintf(V.fs, sizeof V.fs, "%s", choices[i]);
            ui_round(r, 7, on ? ui_alpha(theme.accent, 0.25f) : state ? theme.hover : theme.surface_hi);
            ui_text_box(choices[i], r, 13, FONT_SEMIBOLD, on ? theme.accent : theme.text_dim, ALIGN_CENTER, 0);
            cx += cw + DP(6);
        }
        if (!n) ui_text_box("UDisks2 can make no filesystem here", (Rectangle){x, y, w, ch}, 13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
        y += ch + DP(12);
    }
    if (V.mode != OP_RESIZE && strcmp(V.fs, "swap") && strcmp(V.fs, "none")) {
        y = field_label(x, y, w, "Label");
        int rc = ui_text_field(label_id(), &V.label_f, (Rectangle){x, y, w, DP(38)}, "optional");
        if (rc & TEXT_SUBMIT) return submit(), y;
        if (rc & TEXT_CANCEL) return close_form(), y;
        y += DP(38) + DP(10);
    }
    if (V.form_error[0]) y += ui_text_wrapped(V.form_error, (Rectangle){x, y, w, 0}, 13, FONT_REGULAR, theme.danger, 1) * ui_line_height(13) + DP(8);
    float aw = ui_button_width("Add to queue"), cw = ui_button_width("Cancel");
    if (ui_button(ui_id("dview-submit", 0), (Rectangle){x, y, aw, DP(36)}, "Add to queue", 1)) submit();
    if (ui_button(ui_id("dview-cancel", 0), (Rectangle){x + aw + DP(8), y, cw, DP(36)}, "Cancel", 0)) close_form();
    y += DP(36) + DP(10);
    y += ui_text_wrapped("Nothing is written until you press Apply.", (Rectangle){x, y, w, 0}, 12, FONT_REGULAR, theme.text_faint, 1) * ui_line_height(12);
    return y;
}

static void draw_inspector(Rectangle r)
{
    ui_rect(r, theme.surface);
    ui_rect((Rectangle){r.x, r.y, fmaxf(1, roundf(DP(1))), r.height}, theme.border);
    const dk_disk *d = cur_disk();
    const dk_seg *s = cur_seg();
    if (!s) {
        ui_text_wrapped("Select a partition or free space.\nRight-click for actions.", (Rectangle){r.x + DP(24), r.y + r.height * 0.4f, r.width - DP(48), 0},
                        13, FONT_REGULAR, theme.text_faint, 1);
        return;
    }
    float pad = DP(20), x = r.x + pad, w = r.width - 2 * pad;
    Rectangle body = {r.x, r.y, r.width, r.height};
    static float content;
    ui_scroll(ui_id("dview-side", 0), &V.side_scroll, body, content);
    ui_clip_begin(body);
    float y0 = r.y + pad - V.side_scroll.offset, y = y0;
    int free_seg = s->kind == SEG_FREE;
    char title[256];
    if (free_seg) snprintf(title, sizeof title, s->change == CHANGE_DELETE ? "Freed: %s" : "%s", s->change == CHANGE_DELETE ? s->was : "Free space");
    else snprintf(title, sizeof title, "%s", s->label && *s->label ? s->label : s->part_name && *s->part_name ? s->part_name : *s->type_name ? s->type_name : "Partition");
    y += ui_text_wrapped(title, (Rectangle){x, y, w, 0}, 17, FONT_SEMIBOLD, theme.text, 1) * ui_line_height(17) + DP(4);
    char size[32], line[256];
    dk_size_text(s->size, size, sizeof size);
    snprintf(line, sizeof line, "%s%s%s", free_seg ? "" : s->device, free_seg ? "" : "  |  ", size);
    ui_text_box(line, (Rectangle){x, y, w, ui_line_height(13)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    y += ui_line_height(13) + DP(2);
    ui_text_box(d->name, (Rectangle){x, y, w, ui_line_height(13)}, 13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    y += ui_line_height(13) + DP(12);
    if (!free_seg) {
        char group[200];
        snprintf(group, sizeof group, "%s%s", s->group ? s->group : "", s->inferred ? " (likely)" : "");
        float cx = x + chip(x, y, DP(22), group, dview_group_color(s->group_kind)) + DP(6);
        if (s->change) {
            char st[32];
            snprintf(st, sizeof st, "staged: %s", change_names[s->change]);
            chip(cx, y, DP(22), st, theme.accent);
        }
        y += DP(22) + DP(12);
        char used[96] = "", mounts[512] = "";
        if (s->has_used) {
            char a[32], b[32];
            dk_size_text(s->used, a, sizeof a), dk_size_text(s->used_total, b, sizeof b);
            snprintf(used, sizeof used, "%s of %s", a, b);
        }
        for (int i = 0, n = 0; i < s->mounts.n; i++) n += snprintf(mounts + n, sizeof mounts - (size_t)n, "%s%s", i ? ", " : "", s->mounts.v[i]);
        y = detail(x, y, w, "Filesystem", s->fs_type && *s->fs_type ? s->fs_type : "none");
        y = detail(x, y, w, "Label", s->label);
        y = detail(x, y, w, "Used", used);
        y = detail(x, y, w, "Mounted", mounts);
        y = detail(x, y, w, "Type", s->type_name);
        y = detail(x, y, w, "Name", s->part_name);
        y = detail(x, y, w, "UUID", s->uuid);
        if (s->has_used && s->used_total) {
            y += DP(4);
            ui_progress((Rectangle){x, y, w, DP(6)}, (float)((double)s->used / (double)s->used_total), dview_group_color(s->group_kind));
            y += DP(14);
        }
    }
    ui_hline(x, y, w, theme.border);
    y += DP(12);
    if (V.mode) y = draw_form(x, y, w, d, s);
    else {
        action a[8];
        int n = actions_for(d, s, a);
        for (int i = 0; i < n; i++) {
            Rectangle row = {x - DP(8), y, w + DP(16), DP(34)};
            int state, off = a[i].why[0] != 0;
            if (ui_clickable(ui_id("dview-action", i), GetCollisionRec(row, body), &state)) {
                if (off) ui_toast("%s", a[i].why);
                else {
                    trigger(a[i].id);
                    break;
                }
            }
            if (state) ui_round(row, 7, state == 2 ? theme.pressed : theme.hover);
            Color c = off ? theme.text_faint : a[i].danger ? theme.danger : theme.text;
            ui_icon(a[i].icon, (Rectangle){row.x + DP(6), y, DP(24), row.height}, 16, off ? theme.text_faint : theme.text_dim);
            ui_text_box(a[i].text, (Rectangle){row.x + DP(38), y, row.width - DP(80), row.height}, 14, FONT_REGULAR, c, ALIGN_LEFT, 0);
            ui_text_box(a[i].key, (Rectangle){row.x + row.width - DP(40), y, DP(32), row.height}, 12, FONT_REGULAR, theme.text_faint, ALIGN_RIGHT, 0);
            y += row.height;
            if (off) y += ui_text_wrapped(a[i].why, (Rectangle){row.x + DP(38), y, row.width - DP(46), 0}, 12, FONT_REGULAR, theme.text_faint, 1) *
                              ui_line_height(12) + DP(4);
        }
    }
    ui_clip_end();
    content = y - y0 + 2 * pad;
}

/* ---- the queue ---- */

static float queue_height(float w)
{
    if (!V.plan.ops.n && !V.plan.running && !V.plan.error[0]) return 0;
    float h = DP(14) * 2 + DP(32) + (V.plan.running ? DP(12) : 0);
    h += V.plan.ops.n * (ui_line_height(13) + DP(16) + DP(4));
    if (V.plan.error[0]) h += ui_text_wrapped(V.plan.error, (Rectangle){0, 0, w - DP(28), 0}, 13, FONT_REGULAR, BLANK, 0) * ui_line_height(13) + DP(6);
    return h;
}

static void draw_queue(Rectangle r, float full)
{
    ui_round(r, 12, theme.surface);
    float pad = DP(14), x = r.x + pad, w = r.width - 2 * pad;
    ui_scroll(ui_id("dview-queue", 0), &V.queue_scroll, r, full);
    ui_clip_begin(r);
    float y = r.y + pad - V.queue_scroll.offset;
    char title[256];
    if (V.plan.running)
        snprintf(title, sizeof title, "Applying step %d of %d%s%s", V.plan.done + 1 < V.plan.total ? V.plan.done + 1 : V.plan.total, V.plan.total,
                 V.run.text[0] ? "  |  " : "", V.run.text);
    else snprintf(title, sizeof title, V.plan.ops.n ? "Pending changes (%d)" : "Queue", V.plan.ops.n);
    float bw = 0;
    if (!V.plan.running && V.plan.ops.n) {
        float aw = ui_button_width("Apply..."), dw = ui_button_width("Discard all");
        bw = aw + dw + DP(8);
        if (ui_button(ui_id("dview-apply", 0), (Rectangle){x + w - aw, y, aw, DP(32)}, "Apply...", 2)) text_set(&V.confirm_f, ""), V.confirm = 1;
        if (ui_button(ui_id("dview-discard", 0), (Rectangle){x + w - bw, y, dw, DP(32)}, "Discard all", 0)) dk_plan_clear(&V.plan);
    }
    ui_text_box(title, (Rectangle){x, y, w - bw - DP(8), DP(32)}, 14, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    y += DP(32);
    if (V.plan.running) {
        double step = V.run.progress >= 0 ? V.run.progress : 0;
        ui_progress((Rectangle){x, y + DP(4), w, DP(4)}, (float)((V.plan.done + step) / (V.plan.total ? V.plan.total : 1)), theme.accent);
        y += DP(12);
    }
    float rh = ui_line_height(13) + DP(16);
    for (int i = 0; i < V.plan.ops.n; i++) {
        int current = V.plan.running && i == V.plan.done, done = V.plan.running && i < V.plan.done;
        Rectangle row = {x, y, w, rh};
        ui_round(row, 8, current ? ui_alpha(theme.accent, 0.18f) : done ? ui_alpha(theme.success, 0.12f) : theme.bg);
        char text[600];
        snprintf(text, sizeof text, "%d. %s", i + 1, V.plan.ops.v[i].text ? V.plan.ops.v[i].text : "");
        float rw = ui_text_width("Remove", 12, FONT_SEMIBOLD) + DP(12);
        ui_text_box(text, (Rectangle){x + DP(10), y, w - DP(20) - rw, rh}, 13, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
        if (!V.plan.running) {
            Rectangle rm = {x + w - rw - DP(6), y + DP(4), rw, rh - DP(8)};
            int state;
            if (ui_clickable(ui_id("dview-remove", i), GetCollisionRec(rm, r), &state)) {
                dk_plan_remove(&V.plan, i);
                break;
            }
            ui_text_box("Remove", rm, 12, FONT_SEMIBOLD, state ? theme.danger : theme.text_faint, ALIGN_CENTER, 0);
        }
        y += rh + DP(4);
    }
    if (V.plan.error[0]) ui_text_wrapped(V.plan.error, (Rectangle){x, y + DP(2), w, 0}, 13, FONT_REGULAR, theme.danger, 1);
    ui_clip_end();
}

/* ---- the dialogs and the menu ---- */

static void draw_dialogs(void)
{
    if (V.confirm) {
        char words[128], title[64];
        dk_plan_confirm_words(&V.plan, words, sizeof words);
        snprintf(title, sizeof title, "Apply %d %s", V.plan.ops.n, V.plan.ops.n == 1 ? "change" : "changes");
        size_t len = 256 + strlen(words);
        for (int i = 0; i < V.plan.ops.n; i++) len += strlen(V.plan.ops.v[i].text ? V.plan.ops.v[i].text : "") + 16;
        char *msg = fr_realloc(NULL, len);
        int n = 0;
        for (int i = 0; i < V.plan.ops.n; i++) n += snprintf(msg + n, len - (size_t)n, "%d. %s\n", i + 1, V.plan.ops.v[i].text ? V.plan.ops.v[i].text : "");
        snprintf(msg + n, len - (size_t)n, "\nData on deleted or formatted partitions is gone afterwards. Type %s to confirm.", words);
        fr_dialog d = {.title = title, .message = msg, .field = &V.confirm_f, .buttons = {"Cancel", "Apply"}, .nbuttons = 2, .cancel = 0, .danger = 1};
        int c = ui_dialog(&d);
        free(msg);
        char typed[sizeof V.confirm_f.buf];
        memcpy(typed, V.confirm_f.buf, sizeof typed);
        char *s = typed, *e = typed + strlen(typed);
        while (*s == ' ') s++;
        while (e > s && e[-1] == ' ') *--e = 0;
        if (c == 0 || !V.plan.ops.n || V.plan.running) V.confirm = 0;
        else if (c == 1 && !strcmp(s, words)) V.confirm = 0, apply();
        else if (c == 1) ui_toast("Type %s to confirm", words);
    }
    if (V.image) {
        int d = dk_disk_index(&V.plan.disks, V.image_disk);
        char title[160], msg[400];
        snprintf(title, sizeof title, "Write a bootable image to %s", d >= 0 ? V.plan.disks.v[d].device : "the stick");
        snprintf(msg, sizeof msg, "The path of a hybrid .iso or .img (most Linux ISOs; not Windows ones). Everything on %s is erased when the "
                 "queue is applied.", d >= 0 ? V.plan.disks.v[d].name : "it");
        fr_dialog dlg = {.title = title, .message = msg, .field = &V.image_f, .buttons = {"Cancel", "Add to queue"}, .nbuttons = 2, .cancel = 0};
        int c = ui_dialog(&dlg);
        if (c == 0 || d < 0) V.image = 0;
        else if (c == 1) {
            const char *home = getenv("HOME");
            char *path = V.image_f.buf[0] == '~' && home ? fr_format("%s%s", home, V.image_f.buf + 1) : fr_strdup(V.image_f.buf);
            const char *err = dk_plan_stage_image(&V.plan, V.image_disk, path);
            free(path);
            if (err) ui_toast("%s", err);
            else V.image = 0;
        }
    }
}

static void draw_menu(void)
{
    if (!ui_menu_begin(V.menu)) return;
    action a[8];
    int n = actions_for(cur_disk(), cur_seg(), a), chosen = -1;
    for (int i = 0; i < n; i++) {
        if (a[i].id == OP_RESIZE) ui_menu_separator();
        if (ui_menu_item(a[i].text, a[i].icon, -1)) chosen = i;
    }
    ui_menu_end();
    if (chosen >= 0) {
        if (a[chosen].why[0]) ui_toast("%s", a[chosen].why);
        else trigger(a[chosen].id);
    }
}

/* ---- the view ---- */

int dview_is_open(void) { return V.open; }
int dview_modal(void) { return V.open && (V.confirm || V.image); }

void dview_open(void)
{
    V.open = 1;
    app.drawer_open = 0;
    if (!V.menu) V.menu = ui_id("dview-menu", 0);
    if (plat_dm_supported() && (!V.loaded || plat_dm_serial() != V.serial)) reload();
    ui.dirty = 1;
}

void dview_close(void)
{
    V.open = 0;
    close_form();
    usage_close();
    ui.dirty = 1;
}

static void top_bar(Rectangle r)
{
    ui_rect(r, theme.bg);
    float b = DP(ui.touch ? 48 : 36), x = r.x + DP(4), cy = r.y + (r.height - b) / 2;
    if (!app_wide()) {
        if (ui_icon_button(ui_id("dview-drawer", 0), (Rectangle){x, cy, b, b}, "menu", theme.text)) app.drawer_open = 1, places_refresh(1);
        x += b;
    }
    int inside = usage_is_open();
    if (ui_icon_button(ui_id("dview-back", 0), (Rectangle){x, cy, b, b}, "arrow-left", theme.text)) {
        if (inside) usage_back();
        else dview_close();
    }
    x += b + DP(8);
    const char *title = inside ? usage_title() : plat_dm_supported() ? "Disks" : "Storage";
    ui_text_box(title, (Rectangle){x, r.y, fminf(ui_text_width(title, 17, FONT_SEMIBOLD) + DP(4), r.x + r.width - x - DP(16)), r.height}, 17,
                FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    if (plat_dm_supported() && V.available && !inside) {
        float tw = ui_text_width(title, 17, FONT_SEMIBOLD) + DP(16);
        ui_text_box("Changes are queued and written only when you apply them", (Rectangle){x + tw, r.y, r.x + r.width - x - tw - DP(16), r.height},
                    13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    }
    ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
}

void dview_draw(Rectangle r)
{
    float hh = header_height();
    top_bar((Rectangle){r.x, r.y, r.width, hh});
    Rectangle body = {r.x, r.y + hh, r.width, r.height - hh};
    if (!plat_dm_supported()) {
        if (usage_is_open()) usage_draw(body);
        else storage_draw(body);
        return;
    }
    ui_rect(body, theme.bg);
    float side = body.width >= DP(700) ? fminf(DP(330), body.width * 0.4f) : 0;
    Rectangle map = {body.x, body.y, body.width - side, body.height};
    if (side) draw_inspector((Rectangle){map.x + map.width, body.y, side, body.height});
    if (usage_is_open()) {
        usage_draw(map);
    } else {
        float pad = DP(20), kb = DP(30), x = map.x + pad, w = map.width - 2 * pad;
        ui_text_box("h l partition   j k disk   Enter actions   o look inside   u undo   a apply   ? all keys",
                    (Rectangle){x, map.y + map.height - kb, w, kb}, 12, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
        float qfull = queue_height(w), qh = fminf(qfull, (map.height - kb) * 0.5f);
        if (qh > 0) draw_queue((Rectangle){x, map.y + map.height - kb - qh, w, qh}, qfull);
        Rectangle list = {map.x, map.y, map.width, map.height - kb - (qh > 0 ? qh + DP(8) : 0)};
        uint64_t largest = 1;
        float content = DP(16);
        for (int i = 0; i < V.plan.disks.n; i++) {
            if (V.plan.disks.v[i].size > largest) largest = V.plan.disks.v[i].size;
            float h = disk_height(&V.plan.disks.v[i]);
            if (i < MAX_DISKS) V.disk_top[i] = content - DP(16), V.disk_bottom[i] = content + h + DP(8);
            content += h + DP(22);
        }
        ui_scroll(ui_id("dview-map", 0), &V.map_scroll, list, content);
        ui_clip_begin(list);
        float y = list.y + DP(16) - V.map_scroll.offset;
        for (int i = 0; i < V.plan.disks.n; i++) {
            float h = disk_height(&V.plan.disks.v[i]);
            if (y + h >= list.y && y <= list.y + list.height) draw_disk(i, &V.plan.disks.v[i], x, y, w, largest, list);
            y += h + DP(22);
        }
        if (!V.plan.disks.n)
            ui_text_box(V.available ? "No disks found" : "UDisks2 is not running", (Rectangle){x, list.y + DP(16), w, DP(24)}, 15, FONT_REGULAR,
                        theme.text_dim, ALIGN_LEFT, 0);
        ui_clip_end();
        drag_update();
    }
    draw_menu();
    draw_dialogs();
}

/* ---- keys ---- */

static void command(int c)
{
    int p = V.pending;
    V.pending = 0;
    if (p == 'g' && c == 'g') move_disk(-MAX_DISKS);
    else if (p == 'd' && c == 'd') run_action(OP_DELETE);
    else if ((c == 'g' || c == 'd') && !p) V.pending = c;
    else if (c == 'l') move_segment(1);
    else if (c == 'h') move_segment(-1);
    else if (c == 'j') move_disk(1);
    else if (c == 'k') move_disk(-1);
    else if (c == '0' || c == '^') move_segment(-1000);
    else if (c == '$') move_segment(1000);
    else if (c == 'G') move_disk(MAX_DISKS);
    else if (c == ' ') open_menu_at_selection();
    else if (c == 'o') run_action(A_USAGE);
    else if (c == 'm') run_action(A_MOUNT);
    else if (c == 'r') run_action(OP_RESIZE);
    else if (c == 'f') run_action(OP_FORMAT);
    else if (c == 't') run_action(OP_LABEL);
    else if (c == 'n') run_action(OP_CREATE);
    else if (c == 'w' || c == 'E' || c == 'P') ensure_selection(), disk_key(c);
    else if (c == 'u' && V.plan.ops.n && !V.plan.running) dk_plan_remove(&V.plan, V.plan.ops.n - 1);
    else if (c == 'a' && V.plan.ops.n && !V.plan.running) text_set(&V.confirm_f, ""), V.confirm = 1;
    else if (c == '?') app.help_open = 1;
    else if (c == 'q') dview_close();
    ui.dirty = 1;
}

void dview_keys(void)
{
    if (!plat_dm_supported()) {
        if (usage_is_open()) usage_keys();
        else {
            if (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)) dview_close();
            storage_keys();
        }
        return;
    }
    if (usage_is_open()) {
        usage_keys();
        return;
    }
    if (V.mode) {
        if (ui_key(KEY_TAB, 0)) cycle_fs(1);
        if (ui_key(KEY_TAB, MOD_SHIFT)) cycle_fs(-1);
    }
    if (ui.focus) return;
    if (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)) {
        if (V.pending) V.pending = 0;
        else if (V.mode) close_form();
        else dview_close();
    }
    if (ui_key(KEY_RIGHT, 0)) move_segment(1);
    if (ui_key(KEY_LEFT, 0)) move_segment(-1);
    if (ui_key(KEY_DOWN, 0)) move_disk(1);
    if (ui_key(KEY_UP, 0)) move_disk(-1);
    if (ui_key(KEY_HOME, 0)) move_segment(-1000);
    if (ui_key(KEY_END, 0)) move_segment(1000);
    if (ui_key(KEY_ENTER, 0) || ui_key(KEY_KP_ENTER, 0)) open_menu_at_selection();
    for (int i = 0; i < ui.nchars && V.open && !V.mode && !ui.focus && !usage_is_open() && !V.confirm && !V.image; i++) command(ui.chars[i]);
    ui.nchars = 0;
}

/* ---- between frames ---- */

void dview_poll(void)
{
    if (!plat_dm_supported()) {
        usage_poll();
        return;
    }
    if ((V.open || V.plan.running) && (!V.loaded || plat_dm_serial() != V.serial)) reload();
    plat_dm_status(&V.run);
    if (V.plan.running) {
        while (V.plan.running && V.plan.done < V.run.done && V.plan.done < V.plan.total) dk_plan_step_done(&V.plan);
        if (V.run.finished) {
            dk_plan_finish(&V.plan, V.run.error[0] ? V.run.error : NULL);
            usage_forget();
            if (V.run.error[0]) ui_toast("%s", V.plan.error);
            else ui_toast("Changes applied");
        }
        ui_redraw_in(0.25);
        ui.dirty |= V.run.step_done;
    }
    usage_poll();
}

int dview_disk_done(const fr_disk_done *d) { return usage_disk_done(d); }

void dview_shutdown(void)
{
    /* a half-written stick or a half-moved partition is worse than a window that closes late */
    while (V.plan.running) {
        plat_dm_status(&V.run);
        if (V.run.finished) break;
        struct timespec t = {0, 100 * 1000 * 1000};
        nanosleep(&t, NULL);
    }
    usage_shutdown();
}

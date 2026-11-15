/* Look inside: one folder level of a volume, biggest first, each row's bar its share of the folder (of the volume's
 * used space at the top). A volume that is not mounted is mounted read-only for the scan and given back when the
 * scan ends. Also the storage overview, where the platform has no disk manager. */
#define _GNU_SOURCE
#include "disks_view.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEPTH 64
#define RELEASE_TRIES 5

static struct {
    int open;
    char *key, *title;
    unsigned scan_id;
    const dk_node *path[MAX_DEPTH];
    int stack[MAX_DEPTH]; /* the cursor in each folder above, so going up lands where it was */
    int depth, cursor;
    fr_scroll scroll;
    dk_rows rows;
    int pending_g;
    char *own;            /* the block ferret mounted read-only for the running scan */
    char *mounting;       /* a read-only mount asked for and not answered yet */
    struct {
        char *key;
        int attempt;
        double at; /* the next try, 0 asked already */
    } rel[8];
    int nrel;
} U;

int usage_is_open(void) { return U.open; }

static float row_h(void) { return fmaxf(DP(ui.touch ? 44 : 32), ui_line_height(13) + DP(12)); }

/* ---- the mount the scan borrowed ---- */

/* Desktop watchers (indexers, file managers) open a fresh mount for a moment, so a first "target is busy" is normal:
 * it is tried again for a few seconds. */
static void release(const char *key)
{
    for (int i = 0; i < U.nrel; i++)
        if (!strcmp(U.rel[i].key, key)) return;
    if (U.nrel == (int)(sizeof U.rel / sizeof *U.rel)) return;
    U.rel[U.nrel].key = fr_strdup(key), U.rel[U.nrel].attempt = 0, U.rel[U.nrel].at = 0;
    U.nrel++;
    plat_disk_request(DISK_RELEASE, key);
}

static int releasing(const char *key)
{
    for (int i = 0; i < U.nrel; i++)
        if (!strcmp(U.rel[i].key, key)) return i;
    return -1;
}

static void rel_remove(int i)
{
    free(U.rel[i].key);
    U.rel[i] = U.rel[--U.nrel];
}

static void cancel_active(void)
{
    char *k = dk_usage_cancel();
    if (k && U.own && !strcmp(k, U.own)) {
        release(U.own);
        free(U.own), U.own = NULL;
    }
    free(k);
}

static void begin(const char *key, const char *mount_point, const char *title)
{
    cancel_active();
    if (mount_point) {
        dk_usage_start(key, title, mount_point);
        return;
    }
    dk_usage_mounting(key, title);
    free(U.mounting);
    U.mounting = fr_strdup(key);
    plat_disk_request(DISK_MOUNT_RO, key);
}

static void reset_path(void)
{
    U.depth = U.cursor = 0;
    U.scroll = (fr_scroll){0};
}

void usage_open(const char *key, const char *mount_point, const char *title)
{
    U.open = 1;
    free(U.key), U.key = fr_strdup(key);
    free(U.title), U.title = fr_strdup(title);
    reset_path();
    U.scan_id = 0;
    if (!dk_usage_find(key)) begin(key, mount_point, title);
    ui.dirty = 1;
}

void usage_rescan(void)
{
    const char *active = dk_usage_active();
    if (!U.key || (active && !strcmp(active, U.key))) return;
    reset_path();
    char *mount = dview_mount_of(U.key);
    begin(U.key, mount, U.title);
    free(mount);
}

void usage_close(void)
{
    U.open = 0;
    ui.dirty = 1;
}

void usage_forget(void)
{
    cancel_active();
    dk_usage_forget();
    U.open = 0;
}

int usage_disk_done(const fr_disk_done *d)
{
    if (d->op == DISK_MOUNT_RO) {
        if (U.mounting && !strcmp(U.mounting, d->id)) free(U.mounting), U.mounting = NULL;
        dk_scan *s = dk_usage_find(d->id);
        const char *active = dk_usage_active();
        int wanted = s && s->status == SCAN_MOUNTING && active && !strcmp(active, d->id);
        if (!wanted) {
            if (!d->error) release(d->id);
            return 1;
        }
        char *title = fr_strdup(s->title);
        /* mounted meanwhile, by the user or by ferret's own mount not given back yet: that mount is counted, and
         * stays ferret's to give back when it was */
        const char *at = d->error ? strstr(d->error, "already mounted at `") : NULL;
        if (at) {
            at += strlen("already mounted at `");
            char *path = fr_strndup(at, strcspn(at, "'"));
            int k = releasing(d->id);
            if (k >= 0) {
                rel_remove(k);
                free(U.own), U.own = fr_strdup(d->id);
            }
            dk_usage_start(d->id, title, path);
            free(path);
        } else if (d->error) {
            char *why = fr_format("Could not mount it read-only: %s", d->error);
            dk_usage_fail(d->id, why);
            free(why);
        } else {
            free(U.own), U.own = fr_strdup(d->id);
            dk_usage_start(d->id, title, d->path ? d->path : "");
        }
        free(title);
        ui.dirty = 1;
        return 1;
    }
    if (d->op == DISK_RELEASE) {
        int k = releasing(d->id);
        if (k < 0 || !d->error) {
            if (k >= 0) rel_remove(k);
        } else if (U.rel[k].attempt < RELEASE_TRIES) {
            U.rel[k].attempt++;
            U.rel[k].at = fr_now() + 1;
            ui_redraw_in(1);
        } else {
            const char *name = strrchr(d->id, '/') ? strrchr(d->id, '/') + 1 : d->id;
            ui_toast("Could not unmount %s after counting: %s", name, d->error);
            rel_remove(k);
        }
        return 1;
    }
    return 0;
}

void usage_poll(void)
{
    char *k = dk_usage_poll();
    if (k) {
        if (U.own && !strcmp(U.own, k)) {
            release(U.own);
            free(U.own), U.own = NULL;
        }
        free(k);
        ui.dirty = 1;
    }
    double now = fr_now();
    for (int i = 0; i < U.nrel; i++)
        if (U.rel[i].at && now >= U.rel[i].at) {
            U.rel[i].at = 0;
            plat_disk_request(DISK_RELEASE, U.rel[i].key);
        } else if (U.rel[i].at) ui_redraw_in(U.rel[i].at - now);
    if (U.open && dk_usage_active()) ui_redraw_in(0.25);
}

void usage_shutdown(void)
{
    cancel_active();
    double until = fr_now() + 8;
    while ((U.nrel || U.mounting) && fr_now() < until) {
        fr_disk_done d;
        while (plat_disk_done(&d)) {
            usage_disk_done(&d);
            fr_disk_done_free(&d);
        }
        usage_poll();
        usleep(50 * 1000);
    }
}

/* ---- the view ---- */

static const dk_node *here(void) { return U.depth ? U.path[U.depth - 1] : NULL; }

static void move(int to)
{
    int n = U.rows.n;
    U.cursor = to < 0 ? 0 : to >= n ? (n ? n - 1 : 0) : to;
    float rh = row_h() + DP(3);
    ui_scroll_to(&U.scroll, U.cursor * rh, (U.cursor + 1) * rh);
    ui.dirty = 1;
}

static void enter(int row)
{
    dk_scan *s = dk_usage_find(U.key);
    if (!s || row < 0 || row >= U.rows.n || U.rows.v[row].kind != ROW_DIR || U.depth == MAX_DEPTH) return;
    const dk_node *cur = here() ? here() : s->root;
    dk_usage_lock();
    const dk_node *next = U.rows.v[row].index >= 0 && U.rows.v[row].index < cur->nkids ? cur->kids[U.rows.v[row].index] : NULL;
    dk_usage_unlock();
    if (!next) return;
    U.stack[U.depth] = row;
    U.path[U.depth++] = next;
    U.cursor = 0;
    U.scroll = (fr_scroll){0};
    ui.dirty = 1;
}

static void up(int depth)
{
    if (depth < 0 || depth >= U.depth) return;
    int back = U.stack[depth];
    U.depth = depth;
    dk_usage_rows(dk_usage_find(U.key), here(), &U.rows);
    move(back);
}

static void count_text(uint64_t n, char *buf, size_t len)
{
    if (n >= 1000000) snprintf(buf, len, "%.1f M", (double)n / 1e6);
    else snprintf(buf, len, "%llu", (unsigned long long)n);
}

static int text_link(const char *id, const char *label, float x, float y, float h, float size)
{
    float w = ui_text_width(label, size, FONT_SEMIBOLD);
    Rectangle r = {x - DP(4), y, w + DP(8), h};
    int state, clicked = ui_clickable(ui_id(id, 0), r, &state);
    ui_text_box(label, (Rectangle){x, y, w + DP(2), h}, size, FONT_SEMIBOLD, state ? theme.text : theme.accent, ALIGN_LEFT, 0);
    return clicked;
}

void usage_draw(Rectangle r)
{
    ui_rect(r, theme.bg);
    dk_scan *s = dk_usage_find(U.key);
    if (!s) {
        U.open = 0;
        return;
    }
    if (s->id != U.scan_id) {
        U.scan_id = s->id;
        reset_path();
    }
    dk_usage_rows(s, here(), &U.rows);
    if (U.cursor >= U.rows.n) U.cursor = U.rows.n ? U.rows.n - 1 : 0;

    float pad = DP(ui.touch ? 16 : 20), x = r.x + pad, w = r.width - 2 * pad, y = r.y + DP(14), lh = ui_line_height(16) + DP(6);
    const char *back = plat_dm_supported() ? "< Disks" : "< Storage";
    float bw = ui_text_width(back, 13, FONT_SEMIBOLD) + DP(16);
    if (text_link("usage-back", back, x, y, lh, 13)) usage_close();
    char head[512];
    snprintf(head, sizeof head, "Inside %s", s->title);
    float hw = fminf(ui_text_width(head, 16, FONT_SEMIBOLD) + DP(4), w - bw);
    ui_text_box(head, (Rectangle){x + bw, y, hw, lh}, 16, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    if (s->mount_point)
        ui_text_box(s->mount_point, (Rectangle){x + bw + hw + DP(12), y, w - bw - hw - DP(12), lh}, 13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 1);
    y += lh + DP(4);

    char line[512] = "";
    if (s->status == SCAN_MOUNTING) snprintf(line, sizeof line, "Mounting read-only...");
    else if (s->status != SCAN_ERROR) {
        char used[32], scanned[32], files[32], rest[32];
        dk_size_text(U.rows.used, used, sizeof used), dk_size_text(U.rows.scanned, scanned, sizeof scanned);
        count_text(U.rows.files, files, sizeof files);
        int n = snprintf(line, sizeof line, "%s used  \xc2\xb7  %s in %s files", used, scanned, files);
        if (s->status == SCAN_DONE && U.rows.unaccounted) {
            dk_size_text(U.rows.unaccounted, rest, sizeof rest);
            n += snprintf(line + n, sizeof line - (size_t)n, "  \xc2\xb7  %s not counted", rest);
        }
        if (s->status == SCAN_DONE) {
            struct tm tm;
            char when[16];
            localtime_r(&s->when, &tm);
            strftime(when, sizeof when, "%H:%M", &tm);
            snprintf(line + n, sizeof line - (size_t)n, "  \xc2\xb7  counted %s", when);
        }
    }
    float lw = fminf(ui_text_width(line, 13, FONT_REGULAR) + DP(4), w);
    ui_text_box(line, (Rectangle){x, y, lw, DP(22)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    if ((s->status == SCAN_DONE || s->status == SCAN_ERROR) && text_link("usage-rescan", "Rescan", x + lw + DP(12), y, DP(22), 13)) usage_rescan();
    y += DP(26);
    if (s->status == SCAN_ERROR) {
        int lines = ui_text_wrapped(s->error, (Rectangle){x, y, w, 0}, 13, FONT_REGULAR, theme.danger, 1);
        y += lines * ui_line_height(13) + DP(6);
    }
    if (s->status == SCAN_SCANNING) {
        ui_progress((Rectangle){x, y, w, DP(4)}, U.rows.used ? (float)fmin(1, (double)U.rows.scanned / (double)U.rows.used) : 0, theme.accent);
        y += DP(12);
    }
    if (s->status == SCAN_SCANNING || s->status == SCAN_DONE) {
        float cx = x;
        for (int i = 0; i <= U.depth; i++) {
            const char *name = i ? U.path[i - 1]->name : "/";
            char label[300];
            snprintf(label, sizeof label, "%s%s", name, i && i < U.depth ? "/" : "");
            float cw = ui_text_width(label, 13, FONT_REGULAR) + DP(2);
            if (cx + cw > x + w) break;
            Rectangle cr = {cx, y, cw, DP(24)};
            int state = 0;
            if (i < U.depth && ui_clickable(ui_id("usage-crumb", i), cr, &state)) up(i);
            ui_text_box(label, cr, 13, i == U.depth ? FONT_SEMIBOLD : FONT_REGULAR, i == U.depth ? theme.text : state ? theme.text : theme.accent,
                        ALIGN_LEFT, 0);
            cx += cw + (i ? 0 : DP(2));
        }
        char size[32];
        dk_size_text(U.rows.here, size, sizeof size);
        ui_text_box(size, (Rectangle){cx + DP(12), y, x + w - cx - DP(12), DP(24)}, 13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
        y += DP(30);
    }

    float kb = ui.touch ? 0 : DP(30);
    if (!ui.touch)
        ui_text_box("j k move   l open   h up   gg G top/bottom   r rescan   q back   ? all keys",
                    (Rectangle){x, r.y + r.height - kb, w, kb}, 12, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    Rectangle list = {x, y, w, r.y + r.height - kb - y - DP(4)};
    float rh = row_h(), step = rh + DP(3);
    int note = s->status == SCAN_SCANNING;
    ui_scroll(ui_id("usage-list", 0), &U.scroll, list, U.rows.n * step + (note ? DP(28) : 0));
    ui_clip_begin(list);
    float track_w = fminf(DP(160), w * 0.2f), meta_w = fminf(DP(250), w * 0.4f);
    for (int i = 0; i < U.rows.n; i++) {
        float ry = list.y + i * step - U.scroll.offset;
        if (ry + rh < list.y || ry > list.y + list.height) continue;
        const dk_row *row = &U.rows.v[i];
        Rectangle rr = {list.x, ry, list.width, rh};
        int state, open = row->kind == ROW_DIR, dimmed = row->kind != ROW_DIR && row->kind != ROW_FILE;
        if (ui_clickable(ui_id("usage-row", i), GetCollisionRec(rr, list), &state)) {
            U.cursor = i;
            if (open) enter(i);
        }
        ui_round(rr, 6, i == U.cursor || (open && state) ? theme.surface_hi : theme.surface);
        if (i == U.cursor && !ui.touch) ui_round_line(rr, 6, ui_alpha(theme.accent, 0.7f));
        char name[320];
        snprintf(name, sizeof name, "%s%s%s", row->name, row->kind == ROW_DIR || row->kind == ROW_DENIED ? "/" : "",
                 row->kind == ROW_DENIED ? "  no access" : "");
        float nx = rr.x + DP(10), tx = rr.x + rr.width - DP(10) - meta_w - DP(14) - track_w;
        ui_text_box(name, (Rectangle){nx, ry, tx - nx - DP(12), rh}, 14, FONT_REGULAR, dimmed ? theme.text_dim : theme.text, ALIGN_LEFT, 1);
        Rectangle track = {tx, ry + (rh - DP(6)) / 2, track_w, DP(6)};
        Color fill = dimmed ? theme.text_faint : open ? theme.accent : theme.success;
        ui_round(track, 3, theme.border);
        if (row->size) ui_round((Rectangle){track.x, track.y, fmaxf(DP(6), track_w * (float)fmin(1, row->share)), track.height}, 3, fill);
        char meta[96], files[32] = "", size[32];
        if (row->kind == ROW_DIR) count_text(row->files, files, sizeof files);
        dk_size_text(row->size, size, sizeof size);
        snprintf(meta, sizeof meta, "%s%s%5.1f%%   %s", files, *files ? " files   " : "", row->share * 100, size);
        ui_text_box(meta, (Rectangle){rr.x + rr.width - DP(10) - meta_w, ry, meta_w, rh}, 13, FONT_REGULAR, theme.text_dim, ALIGN_RIGHT, 0);
    }
    if (note)
        ui_text_box("Counting... folders appear as each one finishes. You can go back meanwhile; counting continues.",
                    (Rectangle){list.x, list.y + U.rows.n * step - U.scroll.offset + DP(4), list.width, DP(22)}, 12, FONT_REGULAR,
                    theme.text_faint, ALIGN_LEFT, 0);
    ui_clip_end();
}

void usage_keys(void)
{
    int page = (int)(U.scroll.view / (row_h() + DP(3)) / 2);
    if (page < 1) page = 1;
    if (ui_key(KEY_DOWN, 0)) move(U.cursor + 1);
    if (ui_key(KEY_UP, 0)) move(U.cursor - 1);
    if (ui_key(KEY_PAGE_DOWN, 0) || ui_key(KEY_D, MOD_CTRL)) move(U.cursor + page);
    if (ui_key(KEY_PAGE_UP, 0) || ui_key(KEY_U, MOD_CTRL)) move(U.cursor - page);
    if (ui_key(KEY_HOME, 0)) move(0);
    if (ui_key(KEY_END, 0)) move(U.rows.n - 1);
    if (ui_key(KEY_RIGHT, 0) || ui_key(KEY_ENTER, 0) || ui_key(KEY_KP_ENTER, 0)) enter(U.cursor);
    if (ui_key(KEY_LEFT, 0) || ui_key(KEY_BACKSPACE, 0)) up(U.depth - 1);
    if (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)) {
        if (U.depth && ui.touch) up(U.depth - 1);
        else usage_close();
    }
    for (int i = 0; i < ui.nchars; i++) {
        int c = ui.chars[i], g = U.pending_g;
        U.pending_g = 0;
        if (c == 'j') move(U.cursor + 1);
        else if (c == 'k') move(U.cursor - 1);
        else if (c == 'G') move(U.rows.n - 1);
        else if (c == 'g' && g) move(0);
        else if (c == 'g') U.pending_g = 1;
        else if (c == 'l') enter(U.cursor);
        else if (c == 'h' || c == '-') up(U.depth - 1);
        else if (c == 'r') {
            dk_scan *s = dk_usage_find(U.key);
            if (s && (s->status == SCAN_DONE || s->status == SCAN_ERROR)) usage_rescan();
        } else if (c == 'q') usage_close();
        else if (c == '?') app.help_open = 1;
    }
    ui.nchars = 0;
}

/* ---- the storage overview ---- */

typedef struct {
    int place;
    uint64_t free, total;
    int known;
} volume;

static struct {
    volume v[16];
    int n, cursor;
    double at;
    fr_scroll scroll;
} S;

/* a place no other place holds: a volume, not a folder on one */
static void volumes(void)
{
    if (S.at && ui.now - S.at < 2) return;
    S.at = ui.now;
    S.n = 0;
    for (int i = 0; i < app.places.n && S.n < 16; i++) {
        int inner = 0;
        for (int k = 0; k < app.places.n && !inner; k++)
            inner = k != i && strcmp(app.places.v[k].uri, app.places.v[i].uri) && fr_uri_within(app.places.v[k].uri, app.places.v[i].uri);
        if (inner) continue;
        volume v = {.place = i};
        const fr_backend *b = fr_vfs_backend(app.places.v[i].uri);
        v.known = b && b->free_space && !b->free_space(app.places.v[i].uri, &v.free, &v.total) && v.total;
        S.v[S.n++] = v;
    }
}

static void storage_open(int i)
{
    if (i < 0 || i >= S.n || S.v[i].place >= app.places.n) return;
    const fr_place *p = &app.places.v[S.v[i].place];
    char *path = plat_volume_path(p->uri);
    if (!path) ui_toast("ferret cannot look inside %s", p->label);
    else usage_open(p->uri, path, p->label);
    free(path);
}

void storage_draw(Rectangle r)
{
    ui_rect(r, theme.bg);
    volumes();
    float pad = DP(16), x = r.x + pad, w = r.width - 2 * pad;
    uint64_t largest = 1;
    for (int i = 0; i < S.n; i++)
        if (S.v[i].total > largest) largest = S.v[i].total;
    float card = ui_line_height(16) + ui_line_height(13) + DP(52);
    Rectangle list = {r.x, r.y + DP(8), r.width, r.height - DP(8)};
    ui_scroll(ui_id("storage", 0), &S.scroll, list, S.n * (card + DP(10)) + DP(60));
    ui_clip_begin(list);
    for (int i = 0; i < S.n; i++) {
        const volume *v = &S.v[i];
        if (v->place >= app.places.n) continue;
        const fr_place *p = &app.places.v[v->place];
        float y = list.y + i * (card + DP(10)) - S.scroll.offset;
        Rectangle cr = {x, y, w, card};
        int state;
        if (ui_clickable(ui_id("storage-volume", i), GetCollisionRec(cr, list), &state)) S.cursor = i, storage_open(i);
        ui_round(cr, 12, state == 2 ? theme.surface_hi : theme.surface);
        if (i == S.cursor && !ui.touch) ui_round_line(cr, 12, ui_alpha(theme.accent, 0.7f));
        float ix = cr.x + DP(14), iw = cr.width - DP(28), cy = y + DP(12);
        ui_icon(p->icon, (Rectangle){ix, cy, DP(24), ui_line_height(16)}, 18, theme.accent);
        ui_text_box(p->label, (Rectangle){ix + DP(32), cy, iw - DP(32), ui_line_height(16)}, 16, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
        cy += ui_line_height(16) + DP(4);
        char line[96] = "Size not known";
        if (v->known) {
            char fr[32], tot[32];
            dk_size_text(v->free, fr, sizeof fr), dk_size_text(v->total, tot, sizeof tot);
            snprintf(line, sizeof line, "%s free of %s", fr, tot);
        }
        ui_text_box(line, (Rectangle){ix, cy, iw, ui_line_height(13)}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
        cy += ui_line_height(13) + DP(10);
        float bar = iw * fmaxf(0.35f, (float)((double)v->total / (double)largest));
        Rectangle b = {ix, cy, bar, DP(14)};
        ui_round(b, 7, ui_alpha(theme.accent, 0.18f));
        if (v->known && v->total > v->free)
            ui_round((Rectangle){b.x, b.y, fmaxf(DP(14), bar * (float)((double)(v->total - v->free) / (double)v->total)), b.height}, 7,
                     ui_alpha(theme.accent, 0.8f));
    }
    if (!S.n) ui_text_box("No storage found", (Rectangle){x, list.y + DP(8), w, DP(24)}, 15, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
    ui_clip_end();
}

void storage_keys(void)
{
    if (ui_key(KEY_DOWN, 0)) S.cursor = S.cursor + 1 < S.n ? S.cursor + 1 : S.cursor, ui.dirty = 1;
    if (ui_key(KEY_UP, 0)) S.cursor = S.cursor > 0 ? S.cursor - 1 : 0, ui.dirty = 1;
    if (ui_key(KEY_ENTER, 0) || ui_key(KEY_KP_ENTER, 0)) storage_open(S.cursor);
    for (int i = 0; i < ui.nchars; i++) {
        int c = ui.chars[i];
        if (c == 'j' && S.cursor + 1 < S.n) S.cursor++;
        else if (c == 'k' && S.cursor > 0) S.cursor--;
        else if (c == 'l' || c == 'o') storage_open(S.cursor);
        else if (c == 'q') dview_close();
        else if (c == '?') app.help_open = 1;
        ui.dirty = 1;
    }
    ui.nchars = 0;
}

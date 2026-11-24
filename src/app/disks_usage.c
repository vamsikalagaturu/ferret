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

static float row_h(void)
{
    if (ui.touch) return DP(12) + ui_line_height(15) + DP(2) + ui_line_height(13) + DP(8) + DP(6) + DP(12);
    return fmaxf(DP(32), ui_line_height(13) + DP(12));
}

static float row_step(void) { return row_h() + DP(ui.touch ? 8 : 3); }

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
    float rh = row_step();
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

void usage_back(void)
{
    if (U.depth) up(U.depth - 1);
    else usage_close();
}

const char *usage_title(void) { return U.title ? U.title : ""; }

/* the card on top: the used space, the share of it counted so far, when, and Rescan; returns the y under it */
static float summary(const dk_scan *s, float x, float y, float w)
{
    float pad = DP(14), big = ui_line_height(18), small = ui_line_height(13), bs = DP(36);
    Rectangle card = {x, y, w, pad + big + DP(10) + DP(6) + DP(10) + 2 * small + pad};
    ui_round(card, 12, theme.surface);
    float ix = x + pad, iw = w - 2 * pad, cy = y + pad;
    char head[64], l1[128] = "", l2[128] = "";
    char used[32], scanned[32], files[32], rest[32];
    dk_size_text(U.rows.used, used, sizeof used), dk_size_text(U.rows.scanned, scanned, sizeof scanned);
    count_text(U.rows.files, files, sizeof files);
    if (s->status == SCAN_MOUNTING) snprintf(head, sizeof head, "Mounting read-only...");
    else if (s->status == SCAN_ERROR) snprintf(head, sizeof head, "Could not count");
    else {
        snprintf(head, sizeof head, "%s used", used);
        snprintf(l1, sizeof l1, "%s in %s files counted%s", scanned, files, s->status == SCAN_SCANNING ? " so far" : "");
    }
    if (s->status == SCAN_SCANNING) snprintf(l2, sizeof l2, "Counting... you can go back meanwhile");
    else if (s->status == SCAN_DONE) {
        struct tm tm;
        char when[16];
        localtime_r(&s->when, &tm);
        strftime(when, sizeof when, "%H:%M", &tm);
        if (U.rows.unaccounted) dk_size_text(U.rows.unaccounted, rest, sizeof rest), snprintf(l2, sizeof l2, "%s not counted  \xc2\xb7  %s", rest, when);
        else snprintf(l2, sizeof l2, "Counted at %s", when);
    }
    int can_rescan = s->status == SCAN_DONE || s->status == SCAN_ERROR;
    ui_text_box(head, (Rectangle){ix, cy, iw - (can_rescan ? bs + DP(8) : 0), big}, 18, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    if (can_rescan && ui_icon_button(ui_id("usage-rescan", 0), (Rectangle){x + w - DP(8) - bs, y + DP(8), bs, bs}, "refresh-cw", theme.text_dim))
        usage_rescan();
    cy += big + DP(10);
    float done = U.rows.used ? (float)fmin(1, (double)U.rows.scanned / (double)U.rows.used) : 0;
    ui_round((Rectangle){ix, cy, iw, DP(6)}, 3, theme.border);
    if (done > 0) ui_round((Rectangle){ix, cy, fmaxf(DP(6), iw * done), DP(6)}, 3, theme.accent);
    cy += DP(6) + DP(10);
    ui_text_box(l1, (Rectangle){ix, cy, iw, small}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 1);
    ui_text_box(l2, (Rectangle){ix, cy + small, iw, small}, 13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 1);
    return y + card.height;
}

/* the folders down to the one shown, as chips (the volume first); the first ones give way to "..." when they do not
 * fit; the shown folder's size after them */
static float crumbs(const dk_scan *s, float x, float y, float w)
{
    float h = DP(30), gap = DP(18), sw;
    char size[32];
    dk_size_text(U.rows.here, size, sizeof size);
    sw = ui_text_width(size, 13, FONT_REGULAR) + DP(12);
    float widths[MAX_DEPTH + 1], room = w - sw, used = 0;
    for (int i = 0; i <= U.depth; i++) widths[i] = ui_text_width(i ? U.path[i - 1]->name : s->title, 13, FONT_SEMIBOLD) + DP(20);
    int first = U.depth;
    used = widths[first];
    float more = ui_text_width("...", 13, FONT_SEMIBOLD) + DP(20) + gap;
    while (first > 0 && used + gap + widths[first - 1] + (first - 1 > 0 ? more : 0) <= room) used += gap + widths[--first];
    float cx = x;
    if (first > 0) {
        Rectangle cr = {cx, y, more - gap, h};
        int state;
        if (ui_clickable(ui_id("usage-crumb-more", 0), cr, &state)) up(first - 1);
        ui_round(cr, 8, state ? theme.surface_hi : theme.surface);
        ui_text_box("...", cr, 13, FONT_SEMIBOLD, theme.accent, ALIGN_CENTER, 0);
        cx += more;
        ui_text_box("\xe2\x80\xba", (Rectangle){cx - gap, y, gap, h}, 13, FONT_REGULAR, theme.text_faint, ALIGN_CENTER, 0);
    }
    for (int i = first; i <= U.depth; i++) {
        float cw = fminf(widths[i], x + room - cx);
        Rectangle cr = {cx, y, cw, h};
        int state = 0, last = i == U.depth;
        if (!last && ui_clickable(ui_id("usage-crumb", i), cr, &state)) up(i);
        ui_round(cr, 8, last ? ui_mix(theme.surface, theme.accent, 0.22f) : state ? theme.surface_hi : theme.surface);
        ui_text_box(i ? U.path[i - 1]->name : s->title, (Rectangle){cr.x + DP(10), y, cw - DP(20), h}, 13, FONT_SEMIBOLD,
                    last ? theme.text : theme.accent, ALIGN_LEFT, 1);
        cx += cw;
        if (!last) ui_text_box("\xe2\x80\xba", (Rectangle){cx, y, gap, h}, 13, FONT_REGULAR, theme.text_faint, ALIGN_CENTER, 0), cx += gap;
    }
    ui_text_box(size, (Rectangle){cx + DP(12), y, x + w - cx - DP(12), h}, 13, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    return y + h;
}

static void row_name(const dk_row *row, char *buf, size_t len)
{
    snprintf(buf, len, "%s%s", row->name, row->kind == ROW_DENIED ? "  (no access)" : "");
}

static Color row_fill(const dk_row *row)
{
    return row->kind != ROW_DIR && row->kind != ROW_FILE ? theme.text_faint : row->kind == ROW_DIR ? theme.accent : theme.success;
}

static void share_bar(const dk_row *row, Rectangle track)
{
    ui_round(track, 3, theme.border);
    if (row->size) ui_round((Rectangle){track.x, track.y, fmaxf(DP(6), track.width * (float)fmin(1, row->share)), track.height}, 3, row_fill(row));
}

/* name and share; files and size under them; the bar across */
static void row_touch(const dk_row *row, Rectangle rr)
{
    float p = DP(12), l1 = ui_line_height(15), l2 = ui_line_height(13), x = rr.x + p, w = rr.width - 2 * p, y = rr.y + p;
    int dimmed = row->kind != ROW_DIR && row->kind != ROW_FILE;
    char name[320], pct[16], meta[96], files[32] = "", size[32];
    row_name(row, name, sizeof name);
    snprintf(pct, sizeof pct, "%.1f%%", row->share * 100);
    float pw = ui_text_width(pct, 13, FONT_SEMIBOLD) + DP(4);
    ui_text_box(name, (Rectangle){x, y, w - pw - DP(12), l1}, 15, FONT_SEMIBOLD, dimmed ? theme.text_dim : theme.text, ALIGN_LEFT, 1);
    ui_text_box(pct, (Rectangle){x + w - pw, y, pw, l1}, 13, FONT_SEMIBOLD, theme.text_dim, ALIGN_RIGHT, 0);
    y += l1 + DP(2);
    dk_size_text(row->size, size, sizeof size);
    if (row->kind == ROW_DIR) count_text(row->files, files, sizeof files);
    snprintf(meta, sizeof meta, "%s%s%s", files, *files ? (row->files == 1 ? " file  \xc2\xb7  " : " files  \xc2\xb7  ") : "", size);
    ui_text_box(meta, (Rectangle){x, y, w, l2}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 1);
    y += l2 + DP(8);
    share_bar(row, (Rectangle){x, y, w, DP(6)});
}

static void row_desktop(const dk_row *row, Rectangle rr)
{
    float track_w = fminf(DP(160), rr.width * 0.2f), meta_w = fminf(DP(250), rr.width * 0.4f), ry = rr.y, rh = rr.height;
    int dimmed = row->kind != ROW_DIR && row->kind != ROW_FILE;
    char name[320];
    row_name(row, name, sizeof name);
    if (row->kind == ROW_DIR || row->kind == ROW_DENIED) strncat(name, "/", sizeof name - strlen(name) - 1);
    float nx = rr.x + DP(10), tx = rr.x + rr.width - DP(10) - meta_w - DP(14) - track_w;
    ui_text_box(name, (Rectangle){nx, ry, tx - nx - DP(12), rh}, 14, FONT_REGULAR, dimmed ? theme.text_dim : theme.text, ALIGN_LEFT, 1);
    share_bar(row, (Rectangle){tx, ry + (rh - DP(6)) / 2, track_w, DP(6)});
    char meta[96], files[32] = "", size[32];
    if (row->kind == ROW_DIR) count_text(row->files, files, sizeof files);
    dk_size_text(row->size, size, sizeof size);
    snprintf(meta, sizeof meta, "%s%s%5.1f%%   %s", files, *files ? " files   " : "", row->share * 100, size);
    ui_text_box(meta, (Rectangle){rr.x + rr.width - DP(10) - meta_w, ry, meta_w, rh}, 13, FONT_REGULAR, theme.text_dim, ALIGN_RIGHT, 0);
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

    float pad = DP(ui.touch ? 16 : 20), x = r.x + pad, w = r.width - 2 * pad, y = r.y + DP(12);
    y = summary(s, x, y, w) + DP(10);
    if (s->status == SCAN_ERROR) {
        int lines = ui_text_wrapped(s->error, (Rectangle){x, y, w, 0}, 13, FONT_REGULAR, theme.danger, 1);
        y += lines * ui_line_height(13) + DP(10);
    }
    if (s->status == SCAN_SCANNING || s->status == SCAN_DONE) y = crumbs(s, x, y, w) + DP(8);

    float kb = ui.touch ? 0 : DP(30);
    if (!ui.touch)
        ui_text_box("j k move   l open   h up   gg G top/bottom   r rescan   q back   ? all keys",
                    (Rectangle){x, r.y + r.height - kb, w, kb}, 12, FONT_REGULAR, theme.text_faint, ALIGN_LEFT, 0);
    Rectangle list = {x, y, w, r.y + r.height - kb - y - DP(4)};
    float rh = row_h(), step = row_step();
    ui_scroll(ui_id("usage-list", 0), &U.scroll, list, U.rows.n * step);
    ui_clip_begin(list);
    for (int i = 0; i < U.rows.n; i++) {
        float ry = list.y + i * step - U.scroll.offset;
        if (ry + rh < list.y || ry > list.y + list.height) continue;
        const dk_row *row = &U.rows.v[i];
        Rectangle rr = {list.x, ry, list.width, rh};
        int state, open = row->kind == ROW_DIR;
        if (ui_clickable(ui_id("usage-row", i), GetCollisionRec(rr, list), &state)) {
            U.cursor = i;
            if (open) enter(i);
        }
        ui_round(rr, ui.touch ? 10 : 6, (i == U.cursor && !ui.touch) || (open && state) ? theme.surface_hi : theme.surface);
        if (i == U.cursor && !ui.touch) ui_round_line(rr, 6, ui_alpha(theme.accent, 0.7f));
        if (ui.touch) row_touch(row, rr);
        else row_desktop(row, rr);
    }
    ui_clip_end();
}

void usage_keys(void)
{
    int page = (int)(U.scroll.view / row_step() / 2);
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

int storage_space(const char *uri, char *text, size_t len, float *used)
{
    static struct {
        char uri[512];
        uint64_t free, total;
        int known;
        double at;
    } cache[8];
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(cache[i].uri, uri)) {
            slot = i;
            break;
        }
        if (cache[i].at < cache[slot].at) slot = i;
    }
    if (strcmp(cache[slot].uri, uri) || ui.now - cache[slot].at > 2) {
        snprintf(cache[slot].uri, sizeof cache[slot].uri, "%s", uri);
        cache[slot].at = ui.now;
        const fr_backend *b = fr_vfs_backend(uri);
        cache[slot].known = b && b->free_space && !b->free_space(uri, &cache[slot].free, &cache[slot].total) && cache[slot].total;
    }
    if (!cache[slot].known) return 0;
    char fr[32], tot[32];
    dk_size_text(cache[slot].free, fr, sizeof fr), dk_size_text(cache[slot].total, tot, sizeof tot);
    snprintf(text, len, "%s free of %s", fr, tot);
    uint64_t f = cache[slot].free, t = cache[slot].total;
    *used = (float)((double)(f < t ? t - f : 0) / (double)t);
    return 1;
}

void storage_look_inside(const char *uri, const char *label)
{
    char *path = plat_volume_path(uri);
    if (!path) ui_toast("ferret cannot look inside %s", label);
    else {
        dview_open_from_storage_page();
        usage_open(uri, path, label);
    }
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

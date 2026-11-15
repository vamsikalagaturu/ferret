#include "app.h"
#include "jobs/compare.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One comparison runs on a thread of its own; the window may drop it (and its reference) before it ends. */
typedef struct {
    char *left, *right;
    fr_cmp_options opt;
    fr_cmp_items items;
    int64_t scanned;
    int cancel, done, rc, refs;
} cmp_task;

static struct {
    int on;
    fr_cmp_options opt;
    /* what the result (or the task running) is of */
    unsigned left_id, right_id;
    char *left, *right;
    int64_t left_mtime, right_mtime;
    int jobs_seen;
    cmp_task *task;
    fr_cmp_items items;
    int have, rc;
    /* the sync's preview */
    int preview, to_right, mirror;
    fr_steps plan;
    fr_sync_counts counts;
    fr_scroll scroll;
} C = {.opt = {.tolerance = FR_CMP_TOLERANCE}};

static void task_drop(cmp_task *t)
{
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL)) return;
    free(t->left), free(t->right);
    fr_cmp_items_free(&t->items);
    free(t);
}

static void *task_run(void *arg)
{
    cmp_task *t = arg;
    t->rc = fr_cmp_folders(t->left, t->right, &t->opt, &t->items, &t->scanned, &t->cancel);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    plat_wake();
    task_drop(t);
    return NULL;
}

static void task_stop(void)
{
    if (!C.task) return;
    __atomic_store_n(&C.task->cancel, 1, __ATOMIC_RELAXED);
    task_drop(C.task);
    C.task = NULL;
}

static void forget(void)
{
    task_stop();
    fr_cmp_items_free(&C.items);
    C.have = 0;
    free(C.left), free(C.right);
    C.left = C.right = NULL;
    C.left_id = C.right_id = 0;
}

static int64_t mtime_of(const fr_browser *b) { return b->dir_mtime; }

static void start(fr_browser *l, fr_browser *r)
{
    task_stop();
    free(C.left), free(C.right);
    C.left = fr_strdup(l->uri), C.right = fr_strdup(r->uri);
    C.left_id = l->id, C.right_id = r->id;
    C.left_mtime = mtime_of(l), C.right_mtime = mtime_of(r);
    C.jobs_seen = fr_jobs_finished();
    cmp_task *t = fr_realloc(NULL, sizeof *t);
    *t = (cmp_task){.left = fr_strdup(C.left), .right = fr_strdup(C.right), .opt = C.opt, .refs = 2};
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, task_run, t)) {
        t->rc = -EAGAIN, t->done = 1, t->refs = 1;
    }
    pthread_attr_destroy(&attr);
    C.task = t;
    ui.dirty = 1;
}

/* the panes to compare, or 0 with a reason in *why */
static int panes(fr_browser **l, fr_browser **r, const char **why)
{
    *l = *r = NULL;
    if (!app_split()) return *why = "Compare works in the split view (F3)", 0;
    const fr_tab *t = &tabs.v[tabs.cur];
    *l = t->pane[0], *r = t->pane[1];
    if (!(*l)->uri || !(*r)->uri || (*l)->results || (*r)->results) return *why = "Both panes need a folder to compare", 0;
    if (!strcmp((*l)->uri, (*r)->uri)) return *why = "Both panes show the same folder", 0;
    return 1;
}

int compare_on(void) { return C.on; }

void compare_toggle(void)
{
    if (C.on) {
        C.on = 0;
        forget();
        ui.dirty = 1;
        return;
    }
    fr_browser *l, *r;
    const char *why;
    if (!panes(&l, &r, &why)) {
        ui_toast("%s", why);
        return;
    }
    C.on = 1;
    start(l, r);
}

static void set_option(int recursive, int content)
{
    C.opt.recursive = recursive, C.opt.content = content;
    fr_browser *l, *r;
    const char *why;
    if (C.on && panes(&l, &r, &why)) start(l, r);
}

void compare_poll(void)
{
    if (!C.on) return;
    fr_browser *l, *r;
    const char *why;
    if (!panes(&l, &r, &why)) {
        /* leaving the split view ends it; a pane between folders waits */
        if (!app_split()) C.on = 0, forget(), ui.dirty = 1;
        return;
    }
    if (C.task && __atomic_load_n(&C.task->done, __ATOMIC_ACQUIRE)) {
        fr_cmp_items_free(&C.items);
        C.items = C.task->items;
        C.task->items = (fr_cmp_items){0};
        C.rc = C.task->rc;
        C.have = !C.rc;
        task_drop(C.task);
        C.task = NULL;
        if (C.rc && C.rc != -ECANCELED) ui_toast("Could not compare: %s", fr_vfs_error(C.rc));
        ui.dirty = 1;
    }
    int moved = l->id != C.left_id || r->id != C.right_id || !C.left || strcmp(l->uri, C.left) || strcmp(r->uri, C.right);
    int changed = mtime_of(l) != C.left_mtime || mtime_of(r) != C.right_mtime || fr_jobs_finished() != C.jobs_seen;
    if (moved) fr_cmp_items_free(&C.items), C.have = 0;
    if ((moved || changed) && !C.preview) start(l, r);
}

/* br's side of the comparison: 0 left, 1 right, -1 not compared */
static int side_of_br(void)
{
    if (!C.on || !C.have || !br.uri) return -1;
    if (br.id == C.left_id && C.left && !strcmp(br.uri, C.left)) return 0;
    if (br.id == C.right_id && C.right && !strcmp(br.uri, C.right)) return 1;
    return -1;
}

int compare_state(const char *name)
{
    int side = side_of_br();
    if (side < 0) return -1;
    const fr_cmp_item *it = fr_cmp_find(&C.items, name);
    if (!it) return -1;
    return side ? it->right : it->left;
}

static int differs(int s) { return s == CMP_ONLY || s == CMP_NEWER || s == CMP_OLDER || s == CMP_DIFFERENT || s == CMP_INSIDE; }

static void select_now(const char *s, int n)
{
    if (side_of_br() < 0) {
        ui_toast(C.on ? "Wait for the comparison to finish" : "Compare the panes first (=)");
        return;
    }
    browser_clear_selection();
    int k = 0;
    for (int v = 0; v < br.view.n; v++) {
        fr_item *it = browser_item(v);
        if (differs(compare_state(it->e.name))) browser_set_selected(br.view.v[v], 1), k++;
    }
    ui_toast(k ? "%d %s selected" : "Nothing differs here", k, k == 1 ? "item" : "items");
}

void compare_select_differences(void) { app_defer(select_now, NULL, 0); }

/* ---- the sync and its preview ---- */

static void plan_make(void)
{
    fr_steps_free(&C.plan);
    C.plan = fr_sync_plan(&C.items, C.left, C.right, C.to_right, C.mirror, &C.counts);
}

void compare_sync(int to_right)
{
    if (!C.on || !C.have || C.task) {
        ui_toast(C.on ? "Wait for the comparison to finish" : "Compare the panes first (=)");
        return;
    }
    C.to_right = to_right;
    C.mirror = 0;
    C.scroll = (fr_scroll){0};
    plan_make();
    C.preview = 1;
    ui.dirty = 1;
}

int compare_dialog_open(void) { return C.preview; }

static void preview_close(void)
{
    C.preview = 0;
    fr_steps_free(&C.plan);
    ui.dirty = 1;
}

static void preview_run(void)
{
    if (C.plan.n) fr_job_submit_sync(&C.plan, C.to_right ? C.right : C.left);
    preview_close();
}

/* the path below the compared folders */
static const char *below(const char *uri, const char *root)
{
    size_t n = strlen(root);
    const char *p = uri + n;
    while (*p == '/') p++;
    return p;
}

static void preview_row(const fr_step *s, Rectangle r)
{
    const char *from = C.to_right ? C.left : C.right, *to = C.to_right ? C.right : C.left;
    const char *label, *path = s->kind == STEP_TRASH ? below(s->a, to) : below(s->a, from);
    Color c;
    if (s->kind == STEP_COPIED) label = "New", c = theme.accent;
    else if (s->kind == STEP_REPLACED) label = "Replace", c = theme.warning;
    else label = fr_vfs_can_trash(s->a) ? "To trash" : "Delete", c = theme.danger;
    fr_kind k = kind_of(path, s->is_dir);
    ui_icon(kind_icon(k), (Rectangle){r.x, r.y, DP(24), r.height}, 16, kind_color(k));
    float cw = ui_text_width(label, 12, FONT_SEMIBOLD) + DP(16), ch = DP(20);
    Rectangle chip = {r.x + r.width - cw, r.y + (r.height - ch) / 2, cw, ch};
    ui_round(chip, 10, ui_alpha(c, 0.18f));
    ui_text_box(label, chip, 12, FONT_SEMIBOLD, c, ALIGN_CENTER, 0);
    ui_text_box(path, (Rectangle){r.x + DP(32), r.y, r.width - DP(40) - cw, r.height}, 14, FONT_REGULAR, theme.text, ALIGN_LEFT, 1);
}

void compare_draw_dialog(void)
{
    if (!C.preview) return;
    ui_layer(1);
    ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.scrim);
    Rectangle s = ui.safe;
    float pad = DP(24), w = fminf(s.width - DP(32), DP(600)), lh = DP(28), bh = DP(40);
    char *from = fr_uri_name(C.to_right ? C.left : C.right), *to = fr_uri_name(C.to_right ? C.right : C.left);
    char title[600], summary[600], size[32];
    snprintf(title, sizeof title, "Sync %s (%s) to %s (%s)", *from ? from : "/", C.to_right ? "left" : "right", *to ? to : "/",
             C.to_right ? "right" : "left");
    fr_format_size(C.counts.bytes, size, sizeof size);
    int n = snprintf(summary, sizeof summary, "%d new, %d to replace (%s)", C.counts.copy, C.counts.replace, size);
    if (C.mirror) n += snprintf(summary + n, sizeof summary - (size_t)n, ", %d to the trash", C.counts.remove);
    if (!C.mirror && C.counts.skipped)
        n += snprintf(summary + n, sizeof summary - (size_t)n, ". %d %s here but not newer: left alone", C.counts.skipped,
                      C.counts.skipped == 1 ? "differs" : "differ");
    if (!C.mirror && C.counts.extra)
        snprintf(summary + n, sizeof summary - (size_t)n, ". %d only in %s: kept", C.counts.extra, *to ? to : "/");
    float tw = w - 2 * pad, msg_lh = ui_line_height(14);
    int lines = ui_text_wrapped(summary, (Rectangle){0, 0, tw, 0}, 14, FONT_REGULAR, BLANK, 0);
    const char *note = C.opt.recursive ? NULL : "Folders on both sides were not looked into: compare with subfolders to sync inside them.";
    int note_lines = note ? ui_text_wrapped(note, (Rectangle){0, 0, tw, 0}, 13, FONT_REGULAR, BLANK, 0) : 0;
    float list_h = fminf(fmaxf(1, C.plan.n) * lh, fmaxf(lh * 3, s.height * 0.45f));
    float h = pad + ui_line_height(18) + DP(8) + lines * msg_lh + DP(8) + note_lines * ui_line_height(13) + DP(8) + list_h +
              DP(12) + DP(40) + DP(16) + bh + pad;
    h = fminf(h, s.height - DP(32));
    Rectangle card = {s.x + (s.width - w) / 2, s.y + (s.height - h) / 2, w, h};
    ui_round(card, 14, theme.surface_hi);
    ui_round_line(card, 14, theme.border);
    float x = card.x + pad, y = card.y + pad;
    ui_text_box(title, (Rectangle){x, y, tw, ui_line_height(18)}, 18, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 1);
    y += ui_line_height(18) + DP(8);
    ui_text_wrapped(summary, (Rectangle){x, y, tw, 0}, 14, FONT_REGULAR, theme.text_dim, 1);
    y += lines * msg_lh + DP(8);
    if (note) {
        ui_text_wrapped(note, (Rectangle){x, y, tw, 0}, 13, FONT_REGULAR, theme.text_faint, 1);
        y += note_lines * ui_line_height(13);
    }
    y += DP(8);
    float bottom = card.y + h - pad - bh - DP(16) - DP(40) - DP(12);
    Rectangle list = {x, y, tw, fmaxf(lh, bottom - y)};
    ui_hline(x, list.y, tw, theme.border);
    ui_hline(x, list.y + list.height, tw, theme.border);
    ui_scroll(ui_id("sync-list", 0), &C.scroll, list, C.plan.n * lh);
    ui_clip_begin(list);
    if (!C.plan.n)
        ui_text_box("Nothing to copy: this side has nothing missing or newer there", list, 14, FONT_REGULAR, theme.text_faint, ALIGN_CENTER, 0);
    int first = (int)(C.scroll.offset / lh), last = (int)((C.scroll.offset + list.height) / lh);
    for (int i = first; i <= last && i < C.plan.n; i++)
        preview_row(&C.plan.v[i], (Rectangle){x, roundf(list.y + i * lh - C.scroll.offset), tw - DP(14), lh});
    ui_clip_end();
    y = list.y + list.height + DP(12);

    Rectangle row = {x - DP(8), y, tw + DP(16), DP(40)};
    int state;
    if (ui_clickable(ui_id("sync-mirror", 0), row, &state)) C.mirror = !C.mirror, plan_make(), C.scroll = (fr_scroll){0};
    if (state) ui_round(row, 8, state == 2 ? theme.pressed : theme.hover);
    ui_icon(C.mirror ? "square-check" : "square", (Rectangle){row.x + DP(4), y, DP(28), row.height}, 20, C.mirror ? theme.danger : theme.text_dim);
    char mirror[300];
    /* put_aside() in the job deletes for good where the storage has no trash */
    if (fr_vfs_can_trash(C.to_right ? C.right : C.left))
        snprintf(mirror, sizeof mirror, "Mirror: also replace what differs and move what only %s has to the trash", *to ? to : "/");
    else snprintf(mirror, sizeof mirror, "Mirror: also replace what differs and DELETE what only %s has (no trash there)", *to ? to : "/");
    ui_text_box(mirror, (Rectangle){row.x + DP(40), y, row.width - DP(44), row.height}, 14, FONT_REGULAR, theme.text, ALIGN_LEFT, 1);
    y += row.height + DP(16);

    char go[64];
    int total = C.plan.n;
    if (C.mirror) snprintf(go, sizeof go, "Mirror %d %s", total, total == 1 ? "item" : "items");
    else snprintf(go, sizeof go, "Copy %d %s", total, total == 1 ? "item" : "items");
    float gw = ui_button_width(go), cw = ui_button_width("Cancel");
    float by = card.y + h - pad - bh;
    int run = 0, close = 0;
    if (total) {
        if (ui_button(ui_id("sync-go", 0), (Rectangle){card.x + w - pad - gw, by, gw, bh}, go, C.mirror ? 2 : 1)) run = 1;
    } else {
        Rectangle b = {card.x + w - pad - gw, by, gw, bh};
        ui_round(b, 8, theme.border);
        ui_text_box(go, b, 15, FONT_SEMIBOLD, theme.text_faint, ALIGN_CENTER, 0);
    }
    if (ui_button(ui_id("sync-cancel", 0), (Rectangle){card.x + w - pad - gw - DP(8) - cw, by, cw, bh}, "Cancel", 0)) close = 1;
    /* Enter runs a plain sync; a mirror wants its button */
    if (total && !C.mirror && (ui_key(KEY_ENTER, 0) || ui_key(KEY_KP_ENTER, 0))) run = 1;
    if (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)) close = 1;
    ui_layer(0);
    free(from), free(to);
    if (run) preview_run();
    else if (close) preview_close();
}

/* ---- the bar under the panes ---- */

static int toggle(unsigned id, Rectangle r, const char *label, int on)
{
    int state, clicked = ui_clickable(id, r, &state);
    if (state) ui_round(r, 8, state == 2 ? theme.pressed : theme.hover);
    ui_icon(on ? "square-check" : "square", (Rectangle){r.x + DP(4), r.y, DP(24), r.height}, 18, on ? theme.accent : theme.text_dim);
    ui_text_box(label, (Rectangle){r.x + DP(32), r.y, r.width - DP(36), r.height}, 14, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
    return clicked;
}

float compare_bar_draw(Rectangle area)
{
    if (!C.on || !app_split()) return 0;
    float h = DP(52), pad = DP(12), bh = DP(36);
    Rectangle r = {area.x, area.y + area.height - h, area.width, h};
    ui_rect(r, theme.surface);
    ui_hline(r.x, r.y, r.width, theme.border);
    ui_icon("columns-2", (Rectangle){r.x + pad, r.y, DP(28), h}, 18, theme.accent);
    char status[256];
    if (C.task) {
        long long n = (long long)__atomic_load_n(&C.task->scanned, __ATOMIC_RELAXED);
        snprintf(status, sizeof status, "Comparing... %lld %s", n, n == 1 ? "item" : "items");
        ui_redraw_in(0.1);
    } else if (!C.have) snprintf(status, sizeof status, "Not compared");
    else {
        int only[2] = {0}, diff = 0, same = 0;
        for (int i = 0; i < C.items.top; i++) {
            const fr_cmp_item *it = &C.items.v[i];
            if (it->left == CMP_ONLY) only[0]++;
            else if (it->right == CMP_ONLY) only[1]++;
            else if (it->left == CMP_SAME) same++;
            else if (differs(it->left)) diff++;
        }
        snprintf(status, sizeof status, "%d only left, %d only right, %d differ, %d same", only[0], only[1], diff, same);
    }
    float sw = fmaxf(ui_button_width("Sync to right"), ui_button_width("Sync to left")), selw = ui_button_width("Select differences");
    float rw = ui_text_width("Subfolders", 14, FONT_REGULAR) + DP(44), cw = ui_text_width("Content", 14, FONT_REGULAR) + DP(44);
    float right = r.x + r.width - pad, xb = DP(36);
    Rectangle close = {right - xb, r.y + (h - xb) / 2, xb, xb};
    if (ui_icon_button(ui_id("cmp-close", 0), close, "x", theme.text_dim)) compare_toggle();
    right -= xb + DP(8);
    int ready = C.have && !C.task;
    /* buttons right to left; what does not fit is left to the menu and the keys */
    Rectangle b2 = {right - sw, r.y + (h - bh) / 2, sw, bh}, b1 = {b2.x - DP(8) - sw, b2.y, sw, bh};
    Rectangle b0 = {b1.x - DP(8) - selw, b2.y, selw, bh};
    Rectangle t1 = {b0.x - DP(8) - cw, b2.y, cw, bh}, t0 = {t1.x - DP(4) - rw, b2.y, rw, bh};
    float text_x = r.x + pad + DP(36), min_text = DP(160);
    int room_toggles = t0.x - text_x > min_text, room_select = b0.x - text_x > min_text;
    if (ready && ui_button(ui_id("cmp-to-right", 0), b2, "Sync to right", 0)) compare_sync(1);
    if (ready && ui_button(ui_id("cmp-to-left", 0), b1, "Sync to left", 0)) compare_sync(0);
    if (ready && room_select && ui_button(ui_id("cmp-select", 0), b0, "Select differences", 0)) compare_select_differences();
    float text_end = room_select ? b0.x : b1.x;
    if (room_toggles) {
        if (toggle(ui_id("cmp-recursive", 0), t0, "Subfolders", C.opt.recursive)) set_option(!C.opt.recursive, C.opt.content);
        if (toggle(ui_id("cmp-content", 0), t1, "Content", C.opt.content)) set_option(C.opt.recursive, !C.opt.content);
        text_end = t0.x;
    }
    ui_text_box(status, (Rectangle){text_x, r.y, text_end - DP(8) - text_x, h}, 14, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
    return h;
}

void compare_menu_items(void)
{
    if (!app_split()) return;
    if (ui_menu_item("Compare panes", "columns-2", C.on)) compare_toggle();
    if (!C.on) return;
    if (ui_menu_item("Compare subfolders too", "folder", C.opt.recursive)) set_option(!C.opt.recursive, C.opt.content);
    if (ui_menu_item("Compare content", "file-text", C.opt.content)) set_option(C.opt.recursive, !C.opt.content);
    if (ui_menu_item("Select differences", "check-check", -1)) compare_select_differences();
    if (ui_menu_item("Sync left to right", "arrow-right", -1)) compare_sync(1);
    if (ui_menu_item("Sync right to left", "arrow-left", -1)) compare_sync(0);
}

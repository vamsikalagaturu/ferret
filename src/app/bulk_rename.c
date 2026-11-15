#include "app.h"
#include "jobs/rename.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int open, as_text, regex;
    fr_strs uris, names, to; /* to: the new names, as the last preview made them */
    int *is_dir;
    fr_textarea area;
    fr_text find, replace;
    unsigned focus;
    char error[400];
    int changes;
    fr_steps plan;
    fr_scroll scroll;
    int stale;
} bulk;
static bulk b;

static unsigned area_id(void) { return ui_id("bulk-area", 0); }
static unsigned find_id(void) { return ui_id("bulk-find", 0); }
static unsigned replace_id(void) { return ui_id("bulk-replace", 0); }

void bulk_rename_close(void)
{
    fr_strs_free(&b.uris), fr_strs_free(&b.names), fr_strs_free(&b.to);
    free(b.is_dir);
    textarea_free(&b.area);
    fr_steps_free(&b.plan);
    b = (bulk){0};
    ui.focus = 0;
    ui.dirty = 1;
}

int bulk_rename_open(int as_text)
{
    fr_strs uris = browser_target_uris();
    if (!uris.n) {
        fr_strs_free(&uris);
        return 0;
    }
    bulk_rename_close();
    b.open = 1, b.as_text = as_text, b.uris = uris, b.stale = 1;
    b.is_dir = fr_realloc(NULL, sizeof *b.is_dir * (size_t)uris.n);
    size_t len = 1;
    for (int i = 0; i < uris.n; i++) {
        char *name = fr_uri_name(uris.v[i]);
        fr_entry e = {0};
        b.is_dir[i] = !fr_vfs_stat(uris.v[i], &e) && e.is_dir;
        fr_entry_free(&e);
        len += strlen(name) + 1;
        FR_PUSH(b.names, name);
    }
    char *text = fr_realloc(NULL, len), *o = text;
    for (int i = 0; i < uris.n; i++) o += sprintf(o, "%s%s", i ? "\n" : "", b.names.v[i]);
    *o = 0;
    textarea_set(&b.area, text);
    free(text);
    text_set(&b.find, "");
    text_set(&b.replace, "");
    b.focus = as_text ? area_id() : find_id();
    ui.focus = b.focus;
    return 1;
}

/* the new names from the text or the rule, then whether they can all be given */
static void preview(void)
{
    b.stale = 0;
    fr_strs_free(&b.to);
    fr_steps_free(&b.plan);
    b.error[0] = 0;
    b.changes = 0;
    int n = b.uris.n;
    if (b.as_text) {
        const char *s = b.area.buf;
        for (;;) {
            const char *e = strchr(s, '\n');
            if (!e) e = s + strlen(s);
            FR_PUSH(b.to, fr_strndup(s, (size_t)(e - s)));
            if (!*e) break;
            s = e + 1;
        }
        if (b.to.n == n + 1 && !*b.to.v[n]) free(b.to.v[--b.to.n]);
        if (b.to.n != n) {
            snprintf(b.error, sizeof b.error, "%d %s for %d %s: one name per line", b.to.n, b.to.n == 1 ? "line" : "lines", n,
                     n == 1 ? "item" : "items");
            return;
        }
    } else {
        fr_rename_rule r = {.find = b.find.buf, .replace = b.replace.buf, .regex = b.regex};
        if (fr_rename_rule_compile(&r, b.error, sizeof b.error)) return;
        for (int i = 0; i < n; i++) FR_PUSH(b.to, fr_rename_rule_apply(&r, b.names.v[i], b.is_dir[i], i + 1));
        fr_rename_rule_free(&r);
    }
    fr_strs to = {0};
    for (int i = 0; i < n; i++) {
        if (!fr_name_valid(b.to.v[i])) {
            snprintf(b.error, sizeof b.error, "\"%s\" cannot be named \"%s\": a name cannot be empty, \".\", \"..\" or contain \"/\"",
                     b.names.v[i], b.to.v[i]);
            fr_strs_free(&to);
            return;
        }
        char *parent = fr_uri_parent(b.uris.v[i]);
        FR_PUSH(to, fr_uri_join(parent ? parent : b.uris.v[i], b.to.v[i]));
        free(parent);
        b.changes += strcmp(b.to.v[i], b.names.v[i]) != 0;
    }
    fr_rename_plan(&b.uris, &to, &b.plan, b.error, sizeof b.error);
    fr_strs_free(&to);
}

static int apply(void)
{
    if (b.stale) preview();
    if (b.error[0]) {
        ui_toast("%s", b.error);
        return 0;
    }
    if (!b.changes) return 1;
    fr_job_submit_steps(&b.plan, "Renaming");
    browser_clear_selection();
    return 1;
}

static void checkbox(Rectangle row, const char *label, int *on)
{
    int state;
    if (ui_clickable(ui_id("bulk-check", 0), row, &state)) *on = !*on, b.stale = 1;
    if (state) ui_round(row, 8, state == 2 ? theme.pressed : theme.hover);
    ui_icon(*on ? "square-check" : "square", (Rectangle){row.x + DP(4), row.y, DP(28), row.height}, 20, *on ? theme.accent : theme.text_dim);
    ui_text_box(label, (Rectangle){row.x + DP(40), row.y, row.width - DP(44), row.height}, 14, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
}

static void draw_preview(Rectangle r)
{
    float rh = ui_line_height(14) + DP(10);
    int n = b.to.n;
    ui_round(r, 8, theme.surface);
    ui_round_line(r, 8, theme.border);
    Rectangle in = {r.x + DP(1), r.y + DP(1), r.width - DP(2), r.height - DP(2)};
    ui_scroll(ui_id("bulk-preview", 0), &b.scroll, in, n * rh);
    ui_clip_begin(in);
    float half = (in.width - DP(48)) / 2;
    for (int i = (int)(b.scroll.offset / rh); i < n; i++) {
        float y = in.y + i * rh - b.scroll.offset;
        if (y > in.y + in.height) break;
        int changed = strcmp(b.to.v[i], b.names.v[i]) != 0;
        ui_text_box(b.names.v[i], (Rectangle){in.x + DP(12), y, half, rh}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 1);
        ui_icon("arrow-right", (Rectangle){in.x + DP(12) + half, y, DP(24), rh}, 14, theme.text_faint);
        ui_text_box(b.to.v[i], (Rectangle){in.x + DP(36) + half, y, half, rh}, 14, changed ? FONT_SEMIBOLD : FONT_REGULAR,
                    changed ? theme.text : theme.text_faint, ALIGN_LEFT, 1);
    }
    ui_clip_end();
}

int bulk_rename_draw(void)
{
    if (!b.open) return 0;
    if (b.stale) preview();
    ui_layer(1);
    ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.scrim);
    Rectangle s = ui.safe;
    float pad = DP(ui.touch ? 20 : 24), w = fminf(s.width - DP(32), DP(640)), h = fminf(s.height - DP(48), DP(580));
    float x = s.x + (s.width - w) / 2, y = s.y + fmaxf(DP(24), (s.height - h) / 2), inner = w - 2 * pad;
    Rectangle card = {x, y, w, h};
    ui_round(card, 14, theme.surface_hi);
    ui_round_line(card, 14, theme.border);
    float cy = y + pad, lh = ui_line_height(13), bh = DP(40), field_h = DP(44);
    char title[64];
    snprintf(title, sizeof title, "Rename %d %s", b.uris.n, b.uris.n == 1 ? "item" : "items");
    ui_text_box(title, (Rectangle){x + pad, cy, inner, ui_line_height(18)}, 18, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    cy += ui_line_height(18) + DP(8);
    const char *hint = b.as_text ? "One name per line, in the order shown. Ctrl+Enter renames."
                                 : "An empty Find changes the whole name before the extension. {n}, {n:3} and {n:3:10} number the items.";
    int hl = ui_text_wrapped(hint, (Rectangle){x + pad, cy, inner, 0}, 13, FONT_REGULAR, theme.text_dim, 1);
    cy += hl * lh + DP(10);
    float bottom = y + h - pad - bh - DP(12) - lh - DP(8);
    int submit = 0, cancel = 0;
    if (b.as_text) {
        ui.focus = area_id();
        int res = ui_text_area(area_id(), &b.area, (Rectangle){x + pad, cy, inner, bottom - cy});
        if (res & TEXT_CHANGED) b.stale = 1;
        submit |= res & TEXT_SUBMIT, cancel |= res & TEXT_CANCEL;
    } else {
        if (ui_key(KEY_TAB, 0) || ui_key(KEY_TAB, MOD_SHIFT)) ui.focus = b.focus = b.focus == find_id() ? replace_id() : find_id();
        else if (ui.focus != find_id() && ui.focus != replace_id()) ui.focus = b.focus;
        float lw = DP(110);
        const char *labels[] = {"Find", "Replace with"};
        fr_text *fields[] = {&b.find, &b.replace};
        unsigned ids[] = {find_id(), replace_id()};
        for (int i = 0; i < 2; i++) {
            ui_text_box(labels[i], (Rectangle){x + pad, cy, lw, field_h}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
            int res = ui_text_field(ids[i], fields[i], (Rectangle){x + pad + lw, cy, inner - lw, field_h}, i ? "{n:3}" : NULL);
            if (ui.focus == ids[i]) b.focus = ids[i];
            if (res & TEXT_CHANGED) b.stale = 1;
            submit |= res & TEXT_SUBMIT, cancel |= res & TEXT_CANCEL;
            cy += field_h + DP(8);
        }
        checkbox((Rectangle){x + pad - DP(8), cy, inner + DP(16), DP(36)}, "Regular expression (POSIX extended; \\1 puts back a group)", &b.regex);
        cy += DP(44);
        if (bottom - cy > DP(40)) draw_preview((Rectangle){x + pad, cy, inner, bottom - cy});
    }
    char status[480];
    if (b.error[0]) snprintf(status, sizeof status, "%s", b.error);
    else snprintf(status, sizeof status, "%d of %d %s change", b.changes, b.uris.n, b.changes == 1 ? "name" : "names");
    ui_text_box(status, (Rectangle){x + pad, bottom + DP(8), inner, lh}, 13, FONT_REGULAR, b.error[0] ? theme.danger : theme.text_dim,
                ALIGN_LEFT, 1);
    float by = y + h - pad - bh, bx = x + w - pad;
    float rw = ui_button_width("Rename"), cw = ui_button_width("Cancel");
    if (ui_button(ui_id("bulk-ok", 0), (Rectangle){bx - rw, by, rw, bh}, "Rename", 1)) submit = 1;
    if (ui_button(ui_id("bulk-cancel", 0), (Rectangle){bx - rw - DP(8) - cw, by, cw, bh}, "Cancel", 0)) cancel = 1;
    if (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)) cancel = 1;
    ui_layer(0);
    if (submit && apply()) cancel = 1;
    if (cancel) {
        bulk_rename_close();
        return 0;
    }
    return 1;
}

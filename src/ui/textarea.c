#include "platform.h"
#include "ui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 15

void textarea_set(fr_textarea *t, const char *s)
{
    int n = (int)strlen(s);
    if (n + 1 > t->cap) t->cap = n + 64, t->buf = fr_realloc(t->buf, (size_t)t->cap);
    memcpy(t->buf, s, (size_t)n + 1);
    t->len = n;
    t->caret = t->anchor = 0;
    t->scroll_x = t->scroll_y = 0;
}

void textarea_free(fr_textarea *t)
{
    free(t->buf);
    *t = (fr_textarea){0};
}

static int line_start(const fr_textarea *t, int i)
{
    while (i > 0 && t->buf[i - 1] != '\n') i--;
    return i;
}

static int line_end(const fr_textarea *t, int i)
{
    while (i < t->len && t->buf[i] != '\n') i++;
    return i;
}

static int line_of(const fr_textarea *t, int i)
{
    int n = 0;
    for (int k = 0; k < i; k++) n += t->buf[k] == '\n';
    return n;
}

static float x_of(const fr_textarea *t, int i)
{
    int s = line_start(t, i);
    return ui_text_width_n(t->buf + s, i - s, SIZE, FONT_REGULAR);
}

/* the index nearest x on the line starting at s */
static int index_in_line(const fr_textarea *t, int s, float x)
{
    int e = line_end(t, s);
    float prev = 0;
    for (int i = s; i < e;) {
        int n = fr_utf8_next(t->buf, e, i);
        float w = ui_text_width_n(t->buf + s, n - s, SIZE, FONT_REGULAR);
        if (x < (prev + w) / 2) return i;
        prev = w, i = n;
    }
    return e;
}

static int index_at_line(const fr_textarea *t, int line, float x)
{
    int s = 0;
    for (int k = 0; k < line && s <= t->len; k++) {
        int e = line_end(t, s);
        if (e >= t->len) return index_in_line(t, s, x);
        s = e + 1;
    }
    return index_in_line(t, s, x);
}

static void delete_selection(fr_textarea *t)
{
    int a = t->anchor < t->caret ? t->anchor : t->caret, b = t->anchor < t->caret ? t->caret : t->anchor;
    memmove(t->buf + a, t->buf + b, (size_t)(t->len - b + 1));
    t->len -= b - a;
    t->caret = t->anchor = a;
}

static void insert(fr_textarea *t, const char *s, int n)
{
    delete_selection(t);
    if (t->len + n + 1 > t->cap) t->cap = (t->len + n + 1) * 2, t->buf = fr_realloc(t->buf, (size_t)t->cap);
    memmove(t->buf + t->caret + n, t->buf + t->caret, (size_t)(t->len - t->caret + 1));
    memcpy(t->buf + t->caret, s, (size_t)n);
    t->len += n;
    t->caret = t->anchor = t->caret + n;
}

static double caret_moved_at;

int ui_text_area(unsigned id, fr_textarea *t, Rectangle r)
{
    if (!t->buf) textarea_set(t, "");
    int result = 0, focused = ui.focus == id;
    float pad = DP(12), lh = ui_line_height(SIZE);
    Rectangle inner = {r.x + pad, r.y + DP(8), r.width - 2 * pad, r.height - DP(16)};
    if (ui_live() && ui.pressed) {
        if (CheckCollisionPointRec(ui.mouse, r)) {
            ui.focus = id, focused = 1, ui.active = id;
            int line = (int)((ui.mouse.y - inner.y + t->scroll_y) / lh);
            t->caret = t->anchor = index_at_line(t, line < 0 ? 0 : line, ui.mouse.x - inner.x + t->scroll_x);
            caret_moved_at = ui.now;
        } else if (focused) ui.focus = 0, focused = 0;
    }
    if (focused && ui.active == id && ui.down && !ui.pressed) {
        int line = (int)((ui.mouse.y - inner.y + t->scroll_y) / lh);
        t->caret = index_at_line(t, line < 0 ? 0 : line, ui.mouse.x - inner.x + t->scroll_x);
    }
    if (ui_hover(r) && ui.wheel.y) t->scroll_y -= ui.wheel.y * DP(48), ui.dirty = 1;

    int follow = 0;
    if (focused && ui_live()) {
        for (int i = 0; i < ui.nchars; i++) {
            int c = ui.chars[i], before = t->len;
            if (c == 8) {
                if (t->caret == t->anchor) t->anchor = fr_utf8_prev(t->buf, t->caret);
                delete_selection(t);
            } else if (c == 127) {
                if (t->caret == t->anchor) t->anchor = fr_utf8_next(t->buf, t->len, t->caret);
                delete_selection(t);
            } else if (c >= 32) {
                char enc[4];
                insert(t, enc, fr_utf8_encode(c, enc));
            }
            if (t->len != before || c >= 32) result |= TEXT_CHANGED;
            follow = 1;
        }
        for (int i = 0; i < ui.nkeys; i++) {
            int k = ui.keys[i].key, m = ui.keys[i].mods & ~MOD_SHIFT, shift = ui.keys[i].mods & MOD_SHIFT, used = 1;
            int before = t->len;
            if (k == KEY_BACKSPACE && !m) {
                if (t->caret == t->anchor) t->anchor = fr_utf8_prev(t->buf, t->caret);
                delete_selection(t);
            } else if (k == KEY_DELETE && !m) {
                if (t->caret == t->anchor) t->anchor = fr_utf8_next(t->buf, t->len, t->caret);
                delete_selection(t);
            } else if (k == KEY_LEFT && !m) {
                if (!shift && t->caret != t->anchor) t->caret = t->caret < t->anchor ? t->caret : t->anchor;
                else t->caret = fr_utf8_prev(t->buf, t->caret);
                if (!shift) t->anchor = t->caret;
            } else if (k == KEY_RIGHT && !m) {
                if (!shift && t->caret != t->anchor) t->caret = t->caret > t->anchor ? t->caret : t->anchor;
                else t->caret = fr_utf8_next(t->buf, t->len, t->caret);
                if (!shift) t->anchor = t->caret;
            } else if ((k == KEY_UP || k == KEY_DOWN) && !m) {
                int line = line_of(t, t->caret) + (k == KEY_UP ? -1 : 1);
                float x = x_of(t, t->caret);
                if (line < 0) t->caret = 0;
                else t->caret = index_at_line(t, line, x);
                if (!shift) t->anchor = t->caret;
            } else if (k == KEY_HOME && !m) {
                t->caret = line_start(t, t->caret);
                if (!shift) t->anchor = t->caret;
            } else if (k == KEY_END && !m) {
                t->caret = line_end(t, t->caret);
                if (!shift) t->anchor = t->caret;
            } else if (k == KEY_HOME && m == MOD_CTRL) {
                t->caret = 0;
                if (!shift) t->anchor = 0;
            } else if (k == KEY_END && m == MOD_CTRL) {
                t->caret = t->len;
                if (!shift) t->anchor = t->len;
            } else if (k == KEY_A && m == MOD_CTRL) t->anchor = 0, t->caret = t->len;
            else if ((k == KEY_C || k == KEY_X) && m == MOD_CTRL && t->caret != t->anchor) {
                int a = t->anchor < t->caret ? t->anchor : t->caret, b = t->anchor < t->caret ? t->caret : t->anchor;
                char *s = fr_strndup(t->buf + a, (size_t)(b - a));
                plat_set_clipboard(s);
                free(s);
                if (k == KEY_X) delete_selection(t);
            } else if (k == KEY_V && m == MOD_CTRL) {
                char *s = plat_clipboard();
                if (s) {
                    char *o = s;
                    for (char *c = s; *c; c++)
                        if (*c != '\r') *o++ = *c == '\t' ? ' ' : *c;
                    *o = 0;
                    insert(t, s, (int)strlen(s));
                    free(s);
                }
            } else if ((k == KEY_ENTER || k == KEY_KP_ENTER) && m == MOD_CTRL) result |= TEXT_SUBMIT;
            else if ((k == KEY_ENTER || k == KEY_KP_ENTER) && !m) insert(t, "\n", 1);
            else if ((k == KEY_ESCAPE || k == KEY_BACK) && !m) result |= TEXT_CANCEL;
            else used = 0;
            if (used) ui.keys[i].used = 1, caret_moved_at = ui.now, follow = 1;
            if (t->len != before) result |= TEXT_CHANGED;
        }
        if (ui.nchars) caret_moved_at = ui.now;
    }

    int lines = line_of(t, t->len) + 1;
    float content = lines * lh, caret_y = line_of(t, t->caret) * lh, caret_x = x_of(t, t->caret);
    if (follow) {
        if (caret_y < t->scroll_y) t->scroll_y = caret_y;
        if (caret_y + lh > t->scroll_y + inner.height) t->scroll_y = caret_y + lh - inner.height;
        if (caret_x - t->scroll_x > inner.width - DP(2)) t->scroll_x = caret_x - inner.width + DP(2);
        if (caret_x < t->scroll_x) t->scroll_x = caret_x;
    }
    t->scroll_y = fmaxf(0, fminf(t->scroll_y, fmaxf(0, content - inner.height)));
    if (t->scroll_x < 0) t->scroll_x = 0;

    ui_round(r, 8, theme.surface);
    ui_round_line(r, 8, focused ? theme.accent : theme.border);
    ui_clip_begin(inner);
    Font f = ui_font(FONT_REGULAR, SIZE);
    int a = t->anchor < t->caret ? t->anchor : t->caret, b = t->anchor < t->caret ? t->caret : t->anchor;
    int first = (int)(t->scroll_y / lh), s = 0;
    for (int k = 0; k < first && s <= t->len; k++) {
        int e = line_end(t, s);
        s = e < t->len ? e + 1 : t->len + 1;
    }
    char tmp[1024];
    for (int line = first; s <= t->len; line++) {
        float y = inner.y + line * lh - t->scroll_y, ty = y + (lh - (float)f.baseSize) / 2;
        if (y > inner.y + inner.height) break;
        int e = line_end(t, s);
        if (focused && a != b && a <= e && b >= s) {
            int sa = a > s ? a : s, sb = b < e ? b : e;
            float xa = ui_text_width_n(t->buf + s, sa - s, SIZE, FONT_REGULAR), xb = ui_text_width_n(t->buf + s, sb - s, SIZE, FONT_REGULAR);
            if (b > e) xb += DP(6);
            ui_rect((Rectangle){inner.x + xa - t->scroll_x, ty, xb - xa, (float)f.baseSize}, theme.selection);
        }
        int n = e - s < (int)sizeof tmp - 1 ? e - s : (int)sizeof tmp - 1;
        memcpy(tmp, t->buf + s, (size_t)n);
        tmp[n] = 0;
        ui_text(tmp, inner.x - t->scroll_x, ty, SIZE, FONT_REGULAR, theme.text);
        if (focused && t->caret >= s && t->caret <= e) {
            double phase = fmod(ui.now - caret_moved_at, 1.0);
            if (phase < 0.6) ui_rect((Rectangle){roundf(inner.x + caret_x - t->scroll_x), ty, fmaxf(1, roundf(DP(1.5f))), (float)f.baseSize}, theme.accent);
            ui_redraw_in(phase < 0.6 ? 0.6 - phase : 1.0 - phase);
        }
        s = e + 1;
    }
    ui_clip_end();
    return result;
}

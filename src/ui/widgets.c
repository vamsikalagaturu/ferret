#include "platform.h"
#include "ui.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- buttons ---- */

int ui_icon_button(unsigned id, Rectangle r, const char *icon, Color c)
{
    int state, clicked = ui_clickable(id, r, &state);
    if (state) {
        float d = fminf(r.width, r.height);
        DrawCircle((int)(r.x + r.width / 2), (int)(r.y + r.height / 2), d / 2, state == 2 ? theme.pressed : theme.hover);
    }
    ui_icon(icon, r, 22, c);
    return clicked;
}

float ui_button_width(const char *label) { return ui_text_width(label, 15, FONT_SEMIBOLD) + DP(32); }

int ui_button(unsigned id, Rectangle r, const char *label, int primary)
{
    int state, clicked = ui_clickable(id, r, &state);
    Color fill = primary == 2 ? theme.danger : theme.accent;
    if (primary) {
        ui_round(r, 8, state == 2 ? ui_mix(fill, theme.bg, 0.2f) : state ? ui_mix(fill, theme.text, 0.15f) : fill);
        ui_text_box(label, r, 15, FONT_SEMIBOLD, theme.on_accent, ALIGN_CENTER, 0);
    } else {
        if (state) ui_round(r, 8, state == 2 ? theme.pressed : theme.hover);
        ui_text_box(label, r, 15, FONT_SEMIBOLD, theme.accent, ALIGN_CENTER, 0);
    }
    return clicked;
}

void ui_progress(Rectangle r, float fraction, Color c)
{
    ui_round(r, r.height / 2 / ui.scale, theme.border);
    if (fraction > 0)
        ui_round((Rectangle){r.x, r.y, fmaxf(r.height, r.width * fminf(fraction, 1)), r.height}, r.height / 2 / ui.scale, c);
}

/* ---- text fields ---- */

void text_set(fr_text *t, const char *s)
{
    snprintf(t->buf, sizeof t->buf, "%s", s);
    t->len = (int)strlen(t->buf);
    t->caret = t->anchor = t->len;
    t->scroll = 0;
}

void text_select(fr_text *t, int from, int to)
{
    t->anchor = from < 0 ? 0 : from > t->len ? t->len : from;
    t->caret = to < 0 ? 0 : to > t->len ? t->len : to;
}

static void text_delete_selection(fr_text *t)
{
    int a = t->anchor < t->caret ? t->anchor : t->caret, b = t->anchor < t->caret ? t->caret : t->anchor;
    memmove(t->buf + a, t->buf + b, (size_t)(t->len - b + 1));
    t->len -= b - a;
    t->caret = t->anchor = a;
}

static void text_insert(fr_text *t, const char *s, int n)
{
    text_delete_selection(t);
    if (t->len + n >= (int)sizeof t->buf) n = (int)sizeof t->buf - 1 - t->len;
    if (n <= 0) return;
    memmove(t->buf + t->caret + n, t->buf + t->caret, (size_t)(t->len - t->caret + 1));
    memcpy(t->buf + t->caret, s, (size_t)n);
    t->len += n;
    t->caret = t->anchor = t->caret + n;
}

static int index_at(const fr_text *t, float x)
{
    float prev = 0;
    for (int i = 0; i < t->len;) {
        int n = fr_utf8_next(t->buf, t->len, i);
        float w = ui_text_width_n(t->buf, n, 15, FONT_REGULAR);
        if (x < (prev + w) / 2) return i;
        prev = w, i = n;
    }
    return t->len;
}

static double caret_moved_at;

int ui_text_field(unsigned id, fr_text *t, Rectangle r, const char *placeholder)
{
    int result = 0, focused = ui.focus == id;
    float pad = DP(12);
    Rectangle inner = {r.x + pad, r.y, r.width - 2 * pad, r.height};
    if (ui_live() && ui.pressed) {
        if (CheckCollisionPointRec(ui.mouse, r)) {
            ui.focus = id, focused = 1, ui.active = id;
            t->caret = t->anchor = index_at(t, ui.mouse.x - inner.x + t->scroll);
            caret_moved_at = ui.now;
        } else if (focused) ui.focus = 0, focused = 0;
    }
    if (focused && ui.active == id && ui.down && !ui.pressed) t->caret = index_at(t, ui.mouse.x - inner.x + t->scroll);

    if (focused && ui_live()) {
        for (int i = 0; i < ui.nchars; i++) {
            int c = ui.chars[i], before = t->len;
            if (c == 8) {
                if (t->caret == t->anchor) t->anchor = fr_utf8_prev(t->buf, t->caret);
                text_delete_selection(t);
            } else if (c == 127) {
                if (t->caret == t->anchor) t->anchor = fr_utf8_next(t->buf, t->len, t->caret);
                text_delete_selection(t);
            } else if (c >= 32) {
                char enc[4];
                text_insert(t, enc, fr_utf8_encode(c, enc));
            }
            if (t->len != before || c >= 32) result |= TEXT_CHANGED;
        }
        for (int i = 0; i < ui.nkeys; i++) {
            int k = ui.keys[i].key, m = ui.keys[i].mods & ~MOD_SHIFT, shift = ui.keys[i].mods & MOD_SHIFT, used = 1;
            int before = t->len;
            if (k == KEY_BACKSPACE && !m) {
                if (t->caret == t->anchor) t->anchor = fr_utf8_prev(t->buf, t->caret);
                text_delete_selection(t);
            } else if (k == KEY_DELETE && !m) {
                if (t->caret == t->anchor) t->anchor = fr_utf8_next(t->buf, t->len, t->caret);
                text_delete_selection(t);
            } else if (k == KEY_LEFT && !m) {
                if (!shift && t->caret != t->anchor) t->caret = t->anchor = t->caret < t->anchor ? t->caret : t->anchor;
                else t->caret = fr_utf8_prev(t->buf, t->caret);
                if (!shift) t->anchor = t->caret;
            } else if (k == KEY_RIGHT && !m) {
                if (!shift && t->caret != t->anchor) t->caret = t->anchor = t->caret > t->anchor ? t->caret : t->anchor;
                else t->caret = fr_utf8_next(t->buf, t->len, t->caret);
                if (!shift) t->anchor = t->caret;
            } else if (k == KEY_HOME && !m) {
                t->caret = 0;
                if (!shift) t->anchor = 0;
            } else if (k == KEY_END && !m) {
                t->caret = t->len;
                if (!shift) t->anchor = t->len;
            } else if (k == KEY_A && m == MOD_CTRL) text_select(t, 0, t->len);
            else if ((k == KEY_C || k == KEY_X) && m == MOD_CTRL && t->caret != t->anchor) {
                int a = t->anchor < t->caret ? t->anchor : t->caret, b = t->anchor < t->caret ? t->caret : t->anchor;
                char *s = fr_strndup(t->buf + a, (size_t)(b - a));
                plat_set_clipboard(s);
                free(s);
                if (k == KEY_X) text_delete_selection(t);
            } else if (k == KEY_V && m == MOD_CTRL) {
                char *s = plat_clipboard();
                if (s) {
                    for (char *c = s; *c; c++)
                        if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
                    text_insert(t, s, (int)strlen(s));
                    free(s);
                }
            } else if ((k == KEY_ENTER || k == KEY_KP_ENTER) && !m) result |= TEXT_SUBMIT;
            else if ((k == KEY_ESCAPE || k == KEY_BACK) && !m) result |= TEXT_CANCEL;
            else used = 0;
            if (used) ui.keys[i].used = 1, caret_moved_at = ui.now;
            if (t->len != before) result |= TEXT_CHANGED;
        }
        if (ui.nchars) caret_moved_at = ui.now;
    }

    ui_round(r, 8, theme.surface);
    ui_round_line(r, 8, focused ? theme.accent : theme.border);
    float caret_x = ui_text_width_n(t->buf, t->caret, 15, FONT_REGULAR);
    if (caret_x - t->scroll > inner.width - DP(2)) t->scroll = caret_x - inner.width + DP(2);
    if (caret_x < t->scroll) t->scroll = caret_x;
    if (t->scroll < 0) t->scroll = 0;
    ui_clip_begin(inner);
    Font f = ui_font(FONT_REGULAR, 15);
    float ty = r.y + (r.height - (float)f.baseSize) / 2;
    if (!t->len && placeholder && !focused) ui_text(placeholder, inner.x, ty, 15, FONT_REGULAR, theme.text_faint);
    else if (!t->len && placeholder) ui_text(placeholder, inner.x + DP(2), ty, 15, FONT_REGULAR, theme.text_faint);
    if (focused && t->caret != t->anchor) {
        int a = t->anchor < t->caret ? t->anchor : t->caret, b = t->anchor < t->caret ? t->caret : t->anchor;
        float xa = ui_text_width_n(t->buf, a, 15, FONT_REGULAR), xb = ui_text_width_n(t->buf, b, 15, FONT_REGULAR);
        ui_rect((Rectangle){inner.x + xa - t->scroll, ty, xb - xa, (float)f.baseSize}, theme.selection);
    }
    ui_text(t->buf, inner.x - t->scroll, ty, 15, FONT_REGULAR, theme.text);
    if (focused) {
        double phase = fmod(ui.now - caret_moved_at, 1.0);
        if (phase < 0.6) ui_rect((Rectangle){roundf(inner.x + caret_x - t->scroll), ty, fmaxf(1, roundf(DP(1.5f))), (float)f.baseSize}, theme.accent);
        ui_redraw_in(phase < 0.6 ? 0.6 - phase : 1.0 - phase);
    }
    ui_clip_end();
    return result;
}

/* ---- scrolling ---- */

void ui_scroll(unsigned id, fr_scroll *s, Rectangle r, float content)
{
    s->content = content;
    s->view = r.height;
    float max = fmaxf(0, content - r.height);
    if (ui_hover(r) && ui.wheel.y) {
        s->target -= ui.wheel.y * DP(ui.touch ? 60 : 72);
        if (ui.touch) s->offset = s->target;
        s->moved_at = ui.now;
    }
    s->target = fmaxf(0, fminf(s->target, max));
    if (ui.touch || fabsf(s->target - s->offset) < 0.5f) s->offset = s->target;
    else {
        s->offset += (s->target - s->offset) * (1 - expf(-18 * (float)fmax(ui.dt, 1.0 / 120)));
        ui.dirty = 1;
    }
    s->offset = fmaxf(0, fminf(s->offset, max));
    if (max <= 0) return;

    float thick = DP(ui.touch ? 3 : 6), th = fmaxf(DP(32), r.height * r.height / content);
    float ty = r.y + (r.height - th) * (s->offset / max);
    Rectangle thumb = {r.x + r.width - thick - DP(2), ty, thick, th};
    if (!ui.touch) {
        Rectangle grab = {r.x + r.width - DP(14), r.y, DP(14), r.height};
        if (ui_hover(grab) && ui.pressed) {
            ui.active = id;
            s->bar_drag = 1;
            s->bar_grab = CheckCollisionPointRec(ui.mouse, (Rectangle){grab.x, ty, grab.width, th}) ? ui.mouse.y - ty : th / 2;
        }
        if (s->bar_drag && ui.down && ui.active == id) {
            float y = ui.mouse.y - s->bar_grab - r.y;
            s->target = s->offset = fmaxf(0, fminf(max, y / (r.height - th) * max));
        }
        if (!ui.down) s->bar_drag = 0;
        int lit = s->bar_drag || ui_hover(grab);
        ui_round(thumb, thick / 2 / ui.scale, lit ? theme.text_faint : ui_alpha(theme.text_faint, 0.5f));
    } else {
        double since = ui.now - s->moved_at;
        if (since < 1.2) {
            float a = since < 0.8 ? 0.6f : 0.6f * (float)(1.2 - since) / 0.4f;
            ui_round(thumb, thick / 2 / ui.scale, ui_alpha(theme.text_faint, a));
            ui_redraw_in(0.05);
        }
    }
}

void ui_scroll_to(fr_scroll *s, float top, float bottom)
{
    if (top < s->target) s->target = top;
    else if (bottom > s->target + s->view) s->target = bottom - s->view;
}

/* ---- menus ---- */

typedef struct {
    char label[96];
    const char *icon;
    int checked, separator;
} menu_item;

static struct {
    unsigned id;
    Rectangle anchor, rect;
    menu_item items[32];
    int n, hot, prev_layer;
    int fresh; /* opened by this frame's click, which must not count as one outside it */
    /* the item clicked, by label: it fires in the next frame, when the items before it may have come or gone */
    int clicked;
    char clicked_label[96];
} menu;

void ui_menu_open(unsigned id, Rectangle anchor)
{
    menu.id = id, menu.anchor = anchor, menu.clicked = 0, menu.hot = -1, menu.fresh = 1;
    ui.dirty = 1;
}

static void menu_click(int i)
{
    menu.clicked = 1;
    snprintf(menu.clicked_label, sizeof menu.clicked_label, "%s", menu.items[i].label);
    ui.dirty = 1;
}

int ui_menu_is_open(void) { return menu.id != 0; }
void ui_menu_close(void) { menu.id = 0, ui.dirty = 1; }

int ui_menu_begin(unsigned id)
{
    if (menu.id != id) return 0;
    menu.n = 0;
    menu.prev_layer = ui.layer;
    ui_layer(1);
    return 1;
}

int ui_menu_item(const char *label, const char *icon, int checked)
{
    if (menu.n >= 32) return 0;
    int idx = menu.n++;
    menu_item *it = &menu.items[idx];
    snprintf(it->label, sizeof it->label, "%s", label);
    it->icon = icon, it->checked = checked, it->separator = 0;
    if (menu.clicked && !strcmp(menu.clicked_label, it->label)) {
        menu.clicked = 0;
        ui_menu_close();
        return 1;
    }
    return 0;
}

void ui_menu_separator(void)
{
    if (menu.n < 32) menu.items[menu.n++] = (menu_item){.separator = 1};
}

void ui_menu_end(void)
{
    if (menu.clicked) menu.clicked = 0, ui_menu_close();
    if (!menu.id) {
        ui.layer = menu.prev_layer;
        return;
    }
    float ih = DP(ui.touch ? 48 : 34), sh = DP(9), pad = DP(6), w = DP(200);
    int any_icon = 0, any_check = 0;
    for (int i = 0; i < menu.n; i++) {
        if (menu.items[i].separator) continue;
        any_icon |= menu.items[i].icon != NULL;
        any_check |= menu.items[i].checked >= 0;
    }
    float lead = DP(16) + (any_icon ? DP(32) : 0), trail = DP(16) + (any_check ? DP(28) : 0), h = 2 * pad;
    for (int i = 0; i < menu.n; i++) {
        if (menu.items[i].separator) h += sh;
        else {
            h += ih;
            w = fmaxf(w, lead + ui_text_width(menu.items[i].label, 15, FONT_REGULAR) + trail);
        }
    }
    Rectangle s = ui.safe;
    w = fminf(w, s.width - DP(16));
    float x = menu.anchor.x + menu.anchor.width / 2 > s.x + s.width / 2 ? menu.anchor.x + menu.anchor.width - w : menu.anchor.x;
    float y = menu.anchor.y + menu.anchor.height;
    if (y + h > s.y + s.height - DP(8) && menu.anchor.y - h > s.y + DP(8)) y = menu.anchor.y - h;
    x = fmaxf(s.x + DP(8), fminf(x, s.x + s.width - w - DP(8)));
    y = fmaxf(s.y + DP(8), fminf(y, s.y + s.height - h - DP(8)));
    menu.rect = (Rectangle){x, y, w, h};

    ui_round((Rectangle){x + DP(1), y + DP(3), w, h}, 10, ui_alpha(theme.surface, 0.6f));
    ui_round(menu.rect, 10, theme.surface_hi);
    ui_round_line(menu.rect, 10, theme.border);
    float iy = y + pad;
    for (int i = 0; i < menu.n; i++) {
        menu_item *it = &menu.items[i];
        if (it->separator) {
            ui_hline(x + DP(12), iy + sh / 2, w - DP(24), theme.border);
            iy += sh;
            continue;
        }
        Rectangle row = {x + DP(4), iy, w - DP(8), ih};
        int state, click = ui_clickable(ui_id("menu-item", i), row, &state);
        if (state == 1) menu.hot = i;
        if (state || menu.hot == i) ui_round(row, 6, state == 2 ? theme.pressed : theme.hover);
        if (it->icon) ui_icon(it->icon, (Rectangle){x + DP(12), iy, DP(24), ih}, 18, theme.text_dim);
        ui_text_box(it->label, (Rectangle){x + lead, iy, w - lead - trail, ih}, 15, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
        if (it->checked == 1) ui_icon("check", (Rectangle){x + w - DP(40), iy, DP(24), ih}, 18, theme.accent);
        if (click) menu_click(i);
        iy += ih;
    }
    for (int i = 0; i < ui.nkeys; i++) {
        int k = ui.keys[i].key;
        if (ui.keys[i].used || ui.keys[i].mods) continue;
        if (k == KEY_DOWN || k == KEY_UP) {
            int step = k == KEY_DOWN ? 1 : -1;
            /* nothing lit yet: Down starts at the first item, Up at the last */
            int j = menu.hot >= 0 ? menu.hot : step > 0 ? -1 : menu.n;
            for (int tries = 0; tries < menu.n; tries++) {
                j = (j + step + menu.n) % menu.n;
                if (!menu.items[j].separator) break;
            }
            menu.hot = j, ui.keys[i].used = 1, ui.dirty = 1;
        } else if ((k == KEY_ENTER || k == KEY_KP_ENTER) && menu.hot >= 0) menu_click(menu.hot), ui.keys[i].used = 1;
        else if (k == KEY_ESCAPE || k == KEY_BACK) ui_menu_close(), ui.keys[i].used = 1;
    }
    if (!menu.fresh && (ui.pressed || ui.rpressed) && !CheckCollisionPointRec(ui.mouse, menu.rect)) {
        ui_menu_close();
        /* a right click elsewhere opens the menu there, as one click */
        if (ui.rpressed) ui_push_mouse(MOUSE_BUTTON_RIGHT, 1), ui_push_mouse(MOUSE_BUTTON_RIGHT, 0);
    }
    menu.fresh = 0;
    ui.layer = menu.prev_layer;
}

/* ---- dialogs ---- */

int ui_dialog(const fr_dialog *d)
{
    ui_layer(1);
    ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.scrim);
    Rectangle s = ui.safe;
    float pad = DP(ui.touch ? 20 : 24), w = fminf(s.width - DP(32), DP(420)), inner = w - 2 * pad, h = pad;
    float title_h = ui_line_height(18), msg_lh = ui_line_height(15), field_h = DP(44), check_h = DP(40);
    float bh = DP(40), gap = DP(8);
    if (d->title) h += title_h + DP(8);
    int msg_lines = d->message ? ui_text_wrapped(d->message, (Rectangle){0, 0, inner, 0}, 15, FONT_REGULAR, BLANK, 0) : 0;
    if (msg_lines) h += msg_lines * msg_lh + DP(8);
    if (d->field) h += field_h + DP(12);
    if (d->check) h += check_h;
    float buttons_w = 0;
    for (int i = 0; i < d->nbuttons; i++) buttons_w += ui_button_width(d->buttons[i]) + (i ? gap : 0);
    int stacked = buttons_w > inner;
    h += DP(12) + (stacked ? d->nbuttons * (bh + gap) - gap : bh) + pad;
    float x = s.x + (s.width - w) / 2, y = s.y + fmaxf(DP(24), fminf(s.height * 0.2f, (s.height - h) / 2));
    Rectangle card = {x, y, w, h};
    ui_round(card, 14, theme.surface_hi);
    ui_round_line(card, 14, theme.border);

    int chosen = -1;
    float cy = y + pad;
    if (d->title) {
        ui_text_box(d->title, (Rectangle){x + pad, cy, inner, title_h}, 18, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
        cy += title_h + DP(8);
    }
    if (msg_lines) {
        ui_text_wrapped(d->message, (Rectangle){x + pad, cy, inner, 0}, 15, FONT_REGULAR, theme.text_dim, 1);
        cy += msg_lines * msg_lh + DP(8);
    }
    if (d->field) {
        unsigned fid = ui_id("dialog-field", 0);
        ui.focus = fid;
        int r = ui_text_field(fid, d->field, (Rectangle){x + pad, cy + DP(4), inner, field_h}, NULL);
        if (r & TEXT_SUBMIT) chosen = d->nbuttons - 1;
        if (r & TEXT_CANCEL) chosen = d->cancel;
        cy += field_h + DP(12);
    }
    if (d->check) {
        Rectangle row = {x + pad - DP(8), cy, inner + DP(16), check_h};
        int state;
        if (ui_clickable(ui_id("dialog-check", 0), row, &state)) *d->checked = !*d->checked;
        if (state) ui_round(row, 8, state == 2 ? theme.pressed : theme.hover);
        ui_icon(*d->checked ? "square-check" : "square", (Rectangle){row.x + DP(4), cy, DP(28), check_h}, 20,
                *d->checked ? theme.accent : theme.text_dim);
        ui_text_box(d->check, (Rectangle){row.x + DP(40), cy, row.width - DP(44), check_h}, 15, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
        cy += check_h;
    }
    cy += DP(12);
    if (stacked) {
        for (int i = d->nbuttons - 1; i >= 0; i--) {
            int main = i == d->nbuttons - 1;
            if (ui_button(ui_id("dialog-button", i), (Rectangle){x + pad, cy, inner, bh}, d->buttons[i], main ? 1 + d->danger : 0))
                chosen = i;
            cy += bh + gap;
        }
    } else {
        float bx = x + w - pad;
        for (int i = d->nbuttons - 1; i >= 0; i--) {
            float bw = ui_button_width(d->buttons[i]);
            bx -= bw;
            int main = i == d->nbuttons - 1;
            if (ui_button(ui_id("dialog-button", i), (Rectangle){bx, cy, bw, bh}, d->buttons[i], main ? 1 + d->danger : 0))
                chosen = i;
            bx -= gap;
        }
    }
    if (!d->field) {
        if (ui_key(KEY_ENTER, 0) || ui_key(KEY_KP_ENTER, 0)) chosen = d->nbuttons - 1;
    }
    if (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)) chosen = d->cancel;
    ui_layer(0);
    if (chosen >= 0) ui.focus = 0, ui.dirty = 1;
    return chosen;
}

/* ---- toasts ---- */

static char toast_text[256];
static double toast_until;

void ui_toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast_text, sizeof toast_text, fmt, ap);
    va_end(ap);
    toast_until = fr_now() + 3.5;
    ui.dirty = 1;
}

const char *ui_toast_shown(void) { return fr_now() < toast_until ? toast_text : ""; }

void ui_toast_draw(float bottom)
{
    if (ui.now >= toast_until || !toast_text[0]) return;
    float maxw = fminf(ui.safe.width - DP(32), DP(520)), pad = DP(16);
    int lines = ui_text_wrapped(toast_text, (Rectangle){0, 0, maxw - 2 * pad, 0}, 14, FONT_REGULAR, BLANK, 0);
    float tw = lines > 1 ? maxw : ui_text_width(toast_text, 14, FONT_REGULAR) + 2 * pad;
    float th = lines * ui_line_height(14) + DP(20);
    Rectangle r = {ui.safe.x + (ui.safe.width - tw) / 2, bottom - th - DP(16), tw, th};
    ui_round(r, 10, theme.text);
    /* wrapped at the width the lines were counted at: a box fitted to one line must not wrap again on rounding */
    ui_text_wrapped(toast_text, (Rectangle){r.x + pad, r.y + DP(10), maxw - 2 * pad, 0}, 14, FONT_REGULAR, theme.bg, 1);
    ui_redraw_in(toast_until - ui.now);
}

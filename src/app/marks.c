#include "app.h"
#include "jobs/compare.h"
#include "jobs/gitstatus.h"
#include <math.h>
#include <string.h>

/* A row's marks: how it compares with the other pane (a tint over the row and a chip) and what git says of it (a
 * one-letter chip; ignored items dimmed). */

static int compare_style(int state, const char **label, Color *c)
{
    switch (state) {
    case CMP_ONLY: *label = "Only here", *c = theme.accent; return 1;
    case CMP_NEWER: *label = "Newer", *c = theme.success; return 1;
    case CMP_OLDER: *label = "Older", *c = theme.warning; return 1;
    case CMP_DIFFERENT: *label = "Differs", *c = theme.danger; return 1;
    case CMP_INSIDE: *label = "Differs inside", *c = theme.warning; return 1;
    case CMP_SAME: *label = "Same", *c = theme.text_faint; return 1;
    default: return 0;
    }
}

static int git_style(int flags, const char **label, Color *c)
{
    if (flags & GIT_CONFLICT) *label = "C", *c = theme.danger;
    else if (flags & GIT_MODIFIED) *label = "M", *c = theme.warning;
    else if (flags & GIT_STAGED) *label = "S", *c = theme.success;
    else if (flags & GIT_UNTRACKED) *label = "U", *c = theme.accent;
    else return 0;
    return 1;
}

void marks_tint(const fr_item *it, Rectangle r, float radius)
{
    const char *label;
    Color c;
    int s = it->placeholder ? -1 : compare_state(it->e.name);
    if (it->selected || s == CMP_SAME || !compare_style(s, &label, &c)) return;
    Color tint = ui_alpha(c, 0.10f);
    if (radius > 0) ui_round(r, radius, tint);
    else ui_rect(r, tint);
}

static float chip(const char *label, Color c, float right, float cy, int draw)
{
    float h = DP(18), w = fmaxf(h, ui_text_width(label, 11, FONT_SEMIBOLD) + DP(12));
    if (draw) {
        Rectangle r = {right - w, cy - h / 2, w, h};
        ui_round(r, 9, ui_alpha(c, 0.20f));
        ui_text_box(label, r, 11, FONT_SEMIBOLD, c, ALIGN_CENTER, 0);
    }
    return w;
}

float marks_chips(const fr_item *it, Rectangle r, float keep)
{
    if (it->placeholder) return 0;
    const char *cl, *gl;
    Color cc, gc;
    int has_c = compare_style(compare_state(it->e.name), &cl, &cc), has_g = git_style(git_flags(it->e.name), &gl, &gc);
    if (!has_c && !has_g) return 0;
    float gap = DP(6), right = r.x + r.width, cy = r.y + r.height / 2, used = 0;
    float need = (has_g ? chip(gl, gc, 0, 0, 0) : 0) + (has_c ? chip(cl, cc, 0, 0, 0) + (has_g ? gap : 0) : 0) + gap;
    if (need > r.width - keep) has_c = 0, need = has_g ? chip(gl, gc, 0, 0, 0) + gap : 0;
    if (!need) return 0;
    if (has_g) used += chip(gl, gc, right, cy, 1) + gap;
    if (has_c) used += chip(cl, cc, right - used, cy, 1) + gap;
    return used;
}

int marks_dimmed(const fr_item *it) { return !it->placeholder && (git_flags(it->e.name) & GIT_IGNORED); }

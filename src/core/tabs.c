#include "core/tabs.h"
#include "core/util.h"
#include <stdlib.h>
#include <string.h>

struct fr_browser *tabs_current(const fr_tabs *t)
{
    if (t->cur < 0 || t->cur >= t->n) return NULL;
    const fr_tab *tab = &t->v[t->cur];
    return tab->pane[tab->side];
}

struct fr_browser *tabs_other(const fr_tabs *t)
{
    if (!tabs_split_now(t)) return NULL;
    const fr_tab *tab = &t->v[t->cur];
    return tab->pane[!tab->side];
}

int tabs_split_now(const fr_tabs *t) { return t->cur >= 0 && t->cur < t->n && t->v[t->cur].pane[1]; }

int tabs_add(fr_tabs *t, struct fr_browser *b, int select)
{
    int at = t->n ? t->cur + 1 : 0;
    FR_GROW(*t, t->n + 1);
    memmove(t->v + at + 1, t->v + at, sizeof *t->v * (size_t)(t->n - at));
    t->v[at] = (fr_tab){{b, NULL}, 0};
    t->n++;
    if (select || t->n == 1) t->cur = at;
    return at;
}

int tabs_close(fr_tabs *t, int i, struct fr_browser *out[2])
{
    out[0] = out[1] = NULL;
    if (i < 0 || i >= t->n || t->n < 2) return -1;
    out[0] = t->v[i].pane[0], out[1] = t->v[i].pane[1];
    memmove(t->v + i, t->v + i + 1, sizeof *t->v * (size_t)(t->n - i - 1));
    t->n--;
    if (t->cur > i || t->cur >= t->n) t->cur--;
    return 0;
}

void tabs_select(fr_tabs *t, int i)
{
    if (i >= 0 && i < t->n) t->cur = i;
}

void tabs_step(fr_tabs *t, int by)
{
    if (t->n) t->cur = ((t->cur + by) % t->n + t->n) % t->n;
}

int tabs_split(fr_tabs *t, struct fr_browser *b)
{
    if (t->cur < 0 || t->cur >= t->n || t->v[t->cur].pane[1]) return -1;
    t->v[t->cur].pane[1] = b;
    t->v[t->cur].side = 1;
    return 0;
}

struct fr_browser *tabs_unsplit(fr_tabs *t)
{
    if (!tabs_split_now(t)) return NULL;
    fr_tab *tab = &t->v[t->cur];
    struct fr_browser *gone = tab->pane[!tab->side];
    tab->pane[0] = tab->pane[tab->side];
    tab->pane[1] = NULL;
    tab->side = 0;
    return gone;
}

void tabs_switch_side(fr_tabs *t)
{
    if (tabs_split_now(t)) t->v[t->cur].side = !t->v[t->cur].side;
}

int tabs_find(const fr_tabs *t, const struct fr_browser *b, int *side)
{
    for (int i = 0; b && i < t->n; i++)
        for (int s = 0; s < 2; s++)
            if (t->v[i].pane[s] == b) {
                if (side) *side = s;
                return i;
            }
    return -1;
}

void tabs_free(fr_tabs *t)
{
    free(t->v);
    *t = (fr_tabs){0};
}

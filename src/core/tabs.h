#ifndef FR_TABS_H
#define FR_TABS_H
/* The window's tabs, each one folder or two side by side (a split tab, as Dolphin's split view). A pane holds a
 * struct fr_browser, the app's; this keeps only the order and which tab and pane have the keys, so it needs no window.
 * The caller makes the browsers and frees those handed back. */
struct fr_browser;

typedef struct {
    struct fr_browser *pane[2]; /* pane[1] is NULL unless the tab is split */
    int side;                   /* the pane with the keys: 0 left, 1 right */
} fr_tab;

typedef struct {
    fr_tab *v;
    int n, cap;
    int cur;
} fr_tabs;

struct fr_browser *tabs_current(const fr_tabs *t); /* the pane with the keys; NULL without tabs */
struct fr_browser *tabs_other(const fr_tabs *t);   /* the current tab's other pane; NULL when it is not split */
int tabs_split_now(const fr_tabs *t);              /* the current tab is split */
/* b in a new tab after the current one (or at the end: after < 0), made current when select; its index */
int tabs_add(fr_tabs *t, struct fr_browser *b, int select);
/* takes tab i out, its browsers into out[0..1] (NULL when none); the current tab stays, or when it was i, the one
 * after it becomes current, else the one before (vim's :tabclose). Refuses (-1) the last tab and a bad i. */
int tabs_close(fr_tabs *t, int i, struct fr_browser *out[2]);
void tabs_select(fr_tabs *t, int i); /* out of range: nothing */
void tabs_step(fr_tabs *t, int by);  /* next (+1) or previous (-1), wrapping around */
/* b beside the current pane, as the right one, and given the keys; -1 when the tab is split already */
int tabs_split(fr_tabs *t, struct fr_browser *b);
/* the current tab back to the pane with the keys; the other pane's browser, to free (NULL when not split) */
struct fr_browser *tabs_unsplit(fr_tabs *t);
void tabs_switch_side(fr_tabs *t); /* the keys to the other pane of a split tab */
/* where b is: its tab's index and pane (side may be NULL); -1 when no tab has it */
int tabs_find(const fr_tabs *t, const struct fr_browser *b, int *side);
void tabs_free(fr_tabs *t); /* the list only */
#endif

/* src/core/tabs.c: the order of tabs, the current tab and pane, splitting and closing */
#include "check.h"
#include "core/tabs.h"

/* stand-ins for the app's browsers: only their addresses matter here */
struct fr_browser {
    int n;
};
static struct fr_browser b[8];

static void test_add_and_step(void)
{
    fr_tabs t = {0};
    CHECK(tabs_current(&t) == NULL);
    CHECK(tabs_add(&t, &b[0], 0) == 0); /* the first tab is current even when not asked */
    CHECK(tabs_current(&t) == &b[0]);
    CHECK(tabs_add(&t, &b[1], 1) == 1);
    CHECK(tabs_current(&t) == &b[1]);
    /* a background tab goes right after the current one, which stays */
    tabs_select(&t, 0);
    CHECK(tabs_add(&t, &b[2], 0) == 1);
    CHECK(t.n == 3 && t.cur == 0);
    CHECK(t.v[1].pane[0] == &b[2] && t.v[2].pane[0] == &b[1]);
    tabs_step(&t, -1);
    CHECK(t.cur == 2);
    tabs_step(&t, 1);
    CHECK(t.cur == 0);
    tabs_step(&t, 4);
    CHECK(t.cur == 1);
    tabs_select(&t, 7);
    CHECK(t.cur == 1);
    tabs_select(&t, -1);
    CHECK(t.cur == 1);
    tabs_free(&t);
    CHECK(t.n == 0 && !t.v);
}

static void test_close(void)
{
    fr_tabs t = {0};
    for (int i = 0; i < 4; i++) tabs_add(&t, &b[i], 1);
    struct fr_browser *out[2];
    /* closing the current one: the one after it takes over */
    tabs_select(&t, 1);
    CHECK(tabs_close(&t, 1, out) == 0);
    CHECK(out[0] == &b[1] && out[1] == NULL);
    CHECK(t.n == 3 && tabs_current(&t) == &b[2]);
    /* the last one: the one before */
    tabs_select(&t, 2);
    CHECK(tabs_close(&t, 2, out) == 0);
    CHECK(out[0] == &b[3] && tabs_current(&t) == &b[2]);
    /* one before the current one: the current tab stays current */
    CHECK(tabs_close(&t, 0, out) == 0);
    CHECK(out[0] == &b[0] && t.n == 1 && tabs_current(&t) == &b[2]);
    /* never the last tab, nor one that is not there */
    CHECK(tabs_close(&t, 0, out) == -1 && out[0] == NULL && t.n == 1);
    CHECK(tabs_close(&t, 5, out) == -1);
    tabs_free(&t);
}

static void test_split(void)
{
    fr_tabs t = {0};
    tabs_add(&t, &b[0], 1);
    CHECK(!tabs_split_now(&t) && tabs_other(&t) == NULL);
    CHECK(tabs_unsplit(&t) == NULL);
    CHECK(tabs_split(&t, &b[1]) == 0);
    CHECK(tabs_split_now(&t));
    CHECK(tabs_current(&t) == &b[1] && tabs_other(&t) == &b[0]);
    CHECK(tabs_split(&t, &b[2]) == -1);
    tabs_switch_side(&t);
    CHECK(tabs_current(&t) == &b[0] && tabs_other(&t) == &b[1]);
    int side = -1;
    CHECK(tabs_find(&t, &b[1], &side) == 0 && side == 1);
    CHECK(tabs_find(&t, &b[5], &side) == -1);
    CHECK(tabs_find(&t, NULL, NULL) == -1);
    /* another tab is not split, and the split one keeps its side */
    tabs_add(&t, &b[3], 1);
    CHECK(!tabs_split_now(&t) && tabs_current(&t) == &b[3]);
    tabs_switch_side(&t);
    CHECK(tabs_current(&t) == &b[3]);
    tabs_select(&t, 0);
    CHECK(tabs_current(&t) == &b[0]);
    /* unsplitting keeps the pane with the keys, as the only one */
    CHECK(tabs_unsplit(&t) == &b[1]);
    CHECK(!tabs_split_now(&t) && tabs_current(&t) == &b[0] && t.v[0].side == 0);
    /* the right pane with the keys: it stays */
    tabs_split(&t, &b[4]);
    CHECK(tabs_unsplit(&t) == &b[0]);
    CHECK(tabs_current(&t) == &b[4] && t.v[0].pane[0] == &b[4]);
    /* closing a split tab hands back both browsers */
    tabs_split(&t, &b[5]);
    struct fr_browser *out[2];
    CHECK(tabs_close(&t, 0, out) == 0);
    CHECK(out[0] == &b[4] && out[1] == &b[5]);
    CHECK(tabs_current(&t) == &b[3]);
    tabs_free(&t);
}

int main(void)
{
    test_add_and_step();
    test_close();
    test_split();
    return CHECK_DONE();
}

#include "app.h"
#include "jobs/search.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One search runs at a time, into the browser that started it (owner); a tab's results stay after another's search
 * begins, as a list no longer growing. */
static struct {
    unsigned prompting; /* the browser whose header shows the field, 0 none */
    int content;        /* the field is for the text inside files */
    fr_text field;
    unsigned owner;
    fr_search_status st;
    fr_entries pending; /* found, not in the list yet */
    double added_at;
    double typed_at;    /* the field changed then, not searched for yet */
    char *from;         /* the folder shown before the results, to go back to */
    int everywhere;     /* the search is of the home folder, not the folder shown */
} s;

static unsigned field_id(void) { return ui_id("search-field", 0); }

int search_running(void) { return s.st.running && s.owner == br.id; }

/* the last term of this kind, when the results shown are of it */
static void prompt(int content, int everywhere)
{
    if (!br.uri) return;
    if (br.filtering) filter_stop();
    s.prompting = br.id;
    s.content = content;
    if (!br.results) s.everywhere = everywhere;
    text_set(&s.field, br.results && br.search_term && br.search_content == content ? br.search_term : "");
    ui.focus = field_id();
    ui.dirty = 1;
}

void search_prompt(int content) { prompt(content, 0); }
/* a new search session collects the names afresh, so files made since the last one are found */
void search_everywhere(void)
{
    if (!br.results) {
        char *root = plat_home_uri();
        if (root) fr_search_index(root, br.show_hidden);
        free(root);
    }
    prompt(0, 1);
}

static void prompt_stop(void)
{
    s.prompting = 0;
    if (ui.focus == field_id()) ui.focus = 0;
    ui.nchars = 0;
    ui.dirty = 1;
}

/* below the folder shown, or everywhere: the home folder (internal storage on a phone), whatever folder is shown;
 * leaving those results goes back to that folder */
static void begin_now(const char *term, int content)
{
    if (s.everywhere) {
        char *root = plat_home_uri();
        if (!root) return;
        if (!br.results) free(s.from), s.from = br.uri ? fr_strdup(br.uri) : NULL;
        if (!br.uri || strcmp(br.uri, root)) free(br.uri), br.uri = root;
        else free(root);
    }
    if (!br.uri) return;
    fr_entries_free(&s.pending);
    free(br.search_term);
    br.search_term = fr_strdup(term);
    br.search_content = content;
    fr_search_start(br.uri, term, content, br.show_hidden, plat_wake);
    s.owner = br.id;
    s.st = (fr_search_status){.running = 1};
    s.added_at = 0;
    browser_results_begin();
}

static void leave_now(const char *str, int n)
{
    if (s.owner == br.id) {
        fr_search_cancel();
        if (s.from && br.results) free(br.uri), br.uri = s.from, s.from = NULL;
    }
    browser_results_leave();
}

/* typing searches after a pause; an emptied field shows the folder again */
static void typed_poll(void)
{
    if (!s.typed_at || ui.now - s.typed_at < 0.3) {
        if (s.typed_at) ui_redraw_in(0.3 - (ui.now - s.typed_at));
        return;
    }
    s.typed_at = 0;
    if (!s.prompting || s.prompting != br.id) return;
    if (s.field.len) begin_now(s.field.buf, s.content);
    else if (br.results) leave_now(NULL, 0);
}

static void poll_results(void)
{
    fr_browser *b = browser_by_id(s.owner);
    if (!b || !b->results || b->dupes) {
        if (s.st.running) fr_search_cancel();
        s.st.running = 0;
        fr_entries_free(&s.pending);
        return;
    }
    int was = s.st.running;
    fr_search_take(&s.pending, &s.st);
    /* a sort of the whole list each time: a few times a second is enough */
    if (s.pending.n && (!s.st.running || ui.now - s.added_at > 0.25)) {
        fr_browser *at = br_cur;
        unsigned salt = ui.id_salt;
        browser_enter(b);
        browser_results_add(&s.pending);
        br_cur = at;
        ui.id_salt = salt;
        s.added_at = ui.now;
    }
    if (was || s.st.running) ui.dirty = 1;
    if (s.pending.n || s.st.running) ui_redraw_in(0.25);
}

void search_poll(void)
{
    typed_poll();
    poll_results();
}

void search_reveal(void)
{
    fr_item *it = browser_item(br.cursor);
    if (!br.results || !it) return;
    char *uri = fr_uri_join(br.uri, it->e.name), *parent = fr_uri_parent(uri);
    if (parent) {
        free(br.focus_name);
        br.focus_name = fr_uri_name(uri);
        browser_open(parent, 1);
    }
    free(parent), free(uri);
}

void search_stop(void)
{
    fr_search_stop();
    fr_search_index_drop();
    fr_entries_free(&s.pending);
}

int search_header(Rectangle r)
{
    /* the keys went to another pane or tab: the field goes with them */
    if (s.prompting && browser_by_id(s.prompting) != tabs_current(&tabs)) prompt_stop();
    int prompting = s.prompting && s.prompting == br.id;
    if (!prompting && !br.results) return 0;
    if (!prompting && br.dupes) return dupes_header(r);
    float b = DP(ui.touch ? 48 : 36), x = r.x + DP(4), right = r.x + r.width - DP(4), cy = r.y + (r.height - b) / 2;
    if (prompting) {
        int close = ui_icon_button(ui_id("search-close", 0), (Rectangle){x, cy, b, b}, "arrow-left", theme.text);
        x += b + DP(4);
        Rectangle field = {x, r.y + DP(8), right - x - DP(4), r.height - DP(16)};
        ui.focus = field_id();
        int res = ui_text_field(field_id(), &s.field, field, s.content ? "Search inside the files below here" : s.everywhere ? "Search all files" : "Search names below here");
        if (res & TEXT_CHANGED) s.typed_at = ui.now;
        if (close || res & TEXT_CANCEL) {
            s.typed_at = 0;
            prompt_stop();
            app_defer(leave_now, NULL, 0);
        } else if (res & TEXT_SUBMIT && s.field.len) {
            if (s.typed_at || !br.results) app_defer(begin_now, s.field.buf, s.content);
            s.typed_at = 0;
            prompt_stop();
        }
        ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
        return 1;
    }
    int live = s.owner == br.id;
    int running = live && s.st.running;
    if (ui_icon_button(ui_id("results-close", 0), (Rectangle){x, cy, b, b}, "arrow-left", theme.text)) app_defer(leave_now, NULL, 0);
    x += b + DP(8);
    right -= b;
    if (ui_icon_button(ui_id("more", 0), (Rectangle){right, cy, b, b}, "ellipsis-vertical", theme.text))
        ui_menu_open(app.menu_more, (Rectangle){right, cy, b, b});
    right -= b;
    if (running) {
        if (ui_icon_button(ui_id("search-cancel", 0), (Rectangle){right, cy, b, b}, "x", theme.text)) fr_search_cancel();
    } else if (ui_icon_button(ui_id("search-again", 0), (Rectangle){right, cy, b, b}, "search", theme.text))
        search_prompt(br.search_content);
    char *folder = fr_uri_name(br.uri), title[400], line[400];
    snprintf(title, sizeof title, "%s\"%s\" in %s", br.search_content ? "Text " : "", br.search_term ? br.search_term : "", *folder ? folder : "/");
    free(folder);
    int n = live ? s.st.found : br.view.n;
    if (running)
        snprintf(line, sizeof line, "Searching: %d found, %lld files looked at, now in %s", n, (long long)s.st.files,
                 *s.st.current ? s.st.current : "the folder itself");
    else if (!live) snprintf(line, sizeof line, "%d found", n);
    else if (s.st.unreadable)
        snprintf(line, sizeof line, "%d found; %d %s could not be read", n, s.st.unreadable, s.st.unreadable == 1 ? "folder" : "folders");
    else if (n > br.items.n) snprintf(line, sizeof line, "%d found in %lld files, the best %d shown", n, (long long)s.st.files, br.items.n);
    else snprintf(line, sizeof line, "%d found in %lld files", n, (long long)s.st.files);
    float w = right - x - DP(8), l1 = ui_line_height(15), l2 = ui_line_height(13), top = r.y + (r.height - l1 - l2) / 2;
    ui_text_box(title, (Rectangle){x, top, w, l1}, 15, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    ui_text_box(line, (Rectangle){x, top + l1, w, l2}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 1);
    if (running) ui_progress((Rectangle){r.x, r.y + r.height - DP(3), r.width, DP(2)}, (float)(ui.now - (long)ui.now), theme.accent);
    else ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
    return 1;
}

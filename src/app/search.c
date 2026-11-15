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
} s;

static unsigned field_id(void) { return ui_id("search-field", 0); }

int search_running(void) { return s.st.running && s.owner == br.id; }

/* text NULL: the last term of this kind, when the results shown are of it */
static void prompt_with(int content, const char *text)
{
    if (!br.uri) return;
    if (br.filtering) filter_stop();
    s.prompting = br.id;
    s.content = content;
    text_set(&s.field, text ? text : br.results && br.search_term && br.search_content == content ? br.search_term : "");
    ui.focus = field_id();
    ui.dirty = 1;
}

void search_prompt(int content) { prompt_with(content, NULL); }

static void prompt_stop(void)
{
    s.prompting = 0;
    if (ui.focus == field_id()) ui.focus = 0;
    ui.nchars = 0;
    ui.dirty = 1;
}

int find_open(void) { return br.filtering || (s.prompting && s.prompting == br.id); }

static void scope_now(const char *text, int scope)
{
    if (scope) {
        prompt_with(scope - 1, text);
        return;
    }
    prompt_stop();
    filter_start(0);
    text_set(&br.filter, text);
    browser_rebuild_view();
}

/* under the find field: where it looks, the filter of this folder or a search below it; the typed text goes along */
void find_scopes(Rectangle r)
{
    static const char *const labels[] = {"This folder", "Subfolders too", "Inside files"};
    int scope = br.filtering ? 0 : 1 + s.content;
    const char *text = br.filtering ? br.filter.buf : s.field.buf;
    float h = DP(30), x = r.x + DP(12), y = r.y + (r.height - h) / 2;
    for (int i = 0; i < 3; i++) {
        float w = ui_text_width(labels[i], 13, FONT_SEMIBOLD) + DP(24);
        Rectangle c = {x, y, w, h};
        int state, on = i == scope;
        if (ui_clickable(ui_id("find-scope", i), c, &state) && !on) app_defer(scope_now, text, i);
        ui_round(c, 15, on ? ui_mix(theme.bg, theme.accent, 0.25f) : state ? theme.hover : theme.surface);
        ui_text_box(labels[i], c, 13, FONT_SEMIBOLD, on ? theme.accent : theme.text_dim, ALIGN_CENTER, 0);
        x += w + DP(8);
    }
}

static void begin_now(const char *term, int content)
{
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
    if (s.owner == br.id) fr_search_cancel();
    browser_results_leave();
}

void search_poll(void)
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
        if (ui_icon_button(ui_id("search-close", 0), (Rectangle){x, cy, b, b}, "arrow-left", theme.text)) {
            prompt_stop();
            return 1;
        }
        x += b + DP(4);
        Rectangle field = {x, r.y + DP(8), right - x - DP(4), r.height - DP(16)};
        ui.focus = field_id();
        int res = ui_text_field(field_id(), &s.field, field,
                                s.content ? "Text inside the files here and in subfolders" : "Names here and in subfolders (* and ? work)");
        if (res & TEXT_CANCEL) prompt_stop();
        else if (res & TEXT_SUBMIT && s.field.len) {
            app_defer(begin_now, s.field.buf, s.content);
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
    else snprintf(line, sizeof line, "%d found in %lld files", n, (long long)s.st.files);
    float w = right - x - DP(8), l1 = ui_line_height(15), l2 = ui_line_height(13), top = r.y + (r.height - l1 - l2) / 2;
    ui_text_box(title, (Rectangle){x, top, w, l1}, 15, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 0);
    ui_text_box(line, (Rectangle){x, top + l1, w, l2}, 13, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 1);
    if (running) ui_progress((Rectangle){r.x, r.y + r.height - DP(3), r.width, DP(2)}, (float)(ui.now - (long)ui.now), theme.accent);
    else ui_hline(r.x, r.y + r.height - DP(1), r.width, theme.border);
    return 1;
}

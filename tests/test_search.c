#define _GNU_SOURCE
#include "check.h"
#include "core/match.h"
#include "jobs/search.h"
#include <stdlib.h>
#include <unistd.h>

static char *root;

static void put(const char *rel, const char *text, size_t n)
{
    char *uri = fr_format("%s/%s", root, rel);
    int err = 0;
    fr_stream *s = fr_vfs_open_write(uri, 0, &err);
    CHECK(s != NULL);
    if (s) s->write(s, text, n), s->close(s);
    free(uri);
}

static void mk(const char *rel)
{
    char *uri = fr_format("%s/%s", root, rel);
    CHECK(fr_vfs_mkdir(uri) == 0);
    free(uri);
}

static int cmp(const void *a, const void *b) { return strcmp(((const fr_entry *)a)->name, ((const fr_entry *)b)->name); }

/* the results' names, sorted, joined by spaces */
static char *search(const char *term, int content, int hidden)
{
    fr_search_start(root, term, content, hidden, NULL);
    fr_search_wait();
    fr_entries es = {0};
    fr_search_status st;
    fr_search_take(&es, &st);
    CHECK(!st.running && st.found == es.n);
    if (es.n) qsort(es.v, (size_t)es.n, sizeof *es.v, cmp);
    char *out = fr_strdup("");
    for (int i = 0; i < es.n; i++) {
        char *next = fr_format("%s%s%s", out, i ? " " : "", es.v[i].name);
        free(out);
        out = next;
    }
    fr_entries_free(&es);
    return out;
}

#define CHECK_SEARCH(term, content, hidden, want)                                                                      \
    do {                                                                                                               \
        char *got_ = search(term, content, hidden);                                                                    \
        CHECK_STR(got_, want);                                                                                         \
        free(got_);                                                                                                    \
    } while (0)

int main(void)
{
    /* globs: whole names, * and ?, ASCII case ignored, ? takes one UTF-8 character */
    CHECK(fr_glob_match("*.txt", "notes.txt"));
    CHECK(fr_glob_match("*.TXT", "notes.txt"));
    CHECK(!fr_glob_match("*.txt", "notes.txt.bak"));
    CHECK(fr_glob_match("a*b*c", "aXXbYYc"));
    CHECK(fr_glob_match("a*b*c", "abbbc"));
    CHECK(!fr_glob_match("a*b*c", "acb"));
    CHECK(fr_glob_match("?.c", "x.c"));
    CHECK(!fr_glob_match("?.c", "xy.c"));
    CHECK(fr_glob_match("caf?.md", "caf\xc3\xa9.md"));
    CHECK(fr_glob_match("*", ""));
    CHECK(fr_glob_match("**a", "a"));
    CHECK(!fr_glob_match("", "a"));
    CHECK(fr_glob_match("*a*a*a*b", "aaaaaaaaaaaaaaaaaaab"));
    CHECK(!fr_glob_match("*a*a*a*b", "aaaaaaaaaaaaaaaaaaaa"));
    /* a term without wildcards is a substring */
    CHECK(fr_name_matches("port", "Report.pdf"));
    CHECK(!fr_name_matches("port", "rep.pdf"));
    CHECK(fr_name_matches("rep*.pdf", "Report.pdf"));
    CHECK(!fr_name_matches("port*", "Report.pdf"));

    /* numbering */
    char *s = fr_number_expand("photo-{n:3}", 7);
    CHECK_STR(s, "photo-007");
    free(s);
    s = fr_number_expand("{n}-{n:2}", 12);
    CHECK_STR(s, "12-12");
    free(s);
    s = fr_number_expand("img{n:4:100}", 1);
    CHECK_STR(s, "img0100");
    free(s);
    s = fr_number_expand("img{n:0:0}", 3);
    CHECK_STR(s, "img2");
    free(s);
    s = fr_number_expand("{x} {n {n:} {n:a}}", 1);
    CHECK_STR(s, "{x} {n {n:} {n:a}}");
    free(s);

    char tmpl[] = "/tmp/ferret-search-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    root = fr_uri_from_path(tmpl);
    mk("docs");
    mk("docs/old");
    mk(".hidden");
    mk("Reports");
    put("docs/report.txt", "the quick brown fox", 19);
    put("docs/old/Report-2024.txt", "nothing here", 12);
    put("docs/old/notes.md", "QUICK notes", 11);
    put(".hidden/report.txt", "quick", 5);
    put("bin.dat", "quick\0brown", 11);
    /* a match across the 64 KB reads */
    char *big = fr_realloc(NULL, 70000);
    memset(big, 'x', 70000);
    memcpy(big + 65530, "needle", 6);
    put("big.txt", big, 70000);
    free(big);

    CHECK_SEARCH("report", 0, 0, "Reports docs/old/Report-2024.txt docs/report.txt");
    CHECK_SEARCH("report", 0, 1, ".hidden/report.txt Reports docs/old/Report-2024.txt docs/report.txt");
    CHECK_SEARCH("*.md", 0, 0, "docs/old/notes.md");
    CHECK_SEARCH("r*.txt", 0, 0, "docs/old/Report-2024.txt docs/report.txt");
    CHECK_SEARCH("quick", 1, 0, "docs/old/notes.md docs/report.txt");
    CHECK_SEARCH("needle", 1, 0, "big.txt");
    CHECK_SEARCH("nowhere", 1, 0, "");
    char *bin = fr_format("%s/bin.dat", root), *txt = fr_format("%s/docs/report.txt", root);
    CHECK(fr_search_file(bin, "quick") == 0);
    CHECK(fr_search_file(txt, "QUICK") == 1);
    free(bin), free(txt);

    /* a result is a full entry */
    fr_search_start(root, "report.txt", 0, 0, NULL);
    fr_search_wait();
    fr_entries es = {0};
    fr_search_take(&es, NULL);
    CHECK(es.n == 1 && es.n && es.v[0].size == 19 && !es.v[0].is_dir);
    fr_entries_free(&es);

    /* a new search drops the old one's results */
    fr_search_start(root, "o", 0, 1, NULL);
    fr_search_start(root, "notes", 0, 0, NULL);
    CHECK_SEARCH("notes", 0, 0, "docs/old/notes.md");
    fr_search_stop();
    free(root);
    return CHECK_DONE();
}

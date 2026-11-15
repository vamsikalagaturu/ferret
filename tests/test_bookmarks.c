#include "check.h"
#include "core/bookmarks.h"

/* as settings.c makes them: a path becomes a file:// URI, a URI stays, anything else is none */
static char *to_uri(const char *s)
{
    if (*s == '/') return fr_format("file://%s", s);
    return strstr(s, "://") ? fr_strdup(s) : NULL;
}

static const char *shown(const char *uri) { return !strncmp(uri, "file://", 7) ? uri + 7 : NULL; }

int main(void)
{
    fr_bookmarks b = {0};
    CHECK(fr_bookmark_key_valid('a') && fr_bookmark_key_valid('Z') && fr_bookmark_key_valid('0'));
    CHECK(!fr_bookmark_key_valid('\'') && !fr_bookmark_key_valid(' ') && !fr_bookmark_key_valid('.'));
    fr_bookmark_set(&b, 'd', "file:///home/u/Downloads");
    fr_bookmark_set(&b, '1', "sftp://nas/srv");
    fr_bookmark_set(&b, 'D', "file:///home/u/Documents");
    fr_bookmark_set(&b, '\'', "file:///nope");
    CHECK(b.n == 3);
    CHECK(b.v[0].key == '1' && b.v[1].key == 'D' && b.v[2].key == 'd');
    CHECK_STR(fr_bookmark_get(&b, 'd'), "file:///home/u/Downloads");
    CHECK(fr_bookmark_get(&b, 'x') == NULL);
    fr_bookmark_set(&b, 'd', "file:///tmp");
    CHECK(b.n == 3);
    CHECK_STR(fr_bookmark_get(&b, 'd'), "file:///tmp");
    CHECK(fr_bookmark_key_of(&b, "sftp://nas/srv") == '1');
    CHECK(fr_bookmark_key_of(&b, "file:///none") == 0);
    CHECK(fr_bookmark_free_key(&b) == '2');

    /* written into settings.toml and read back */
    fr_toml_out o = {0};
    fr_toml_string(&o, "layout", "grid");
    fr_toml_comment(&o, "");
    fr_bookmarks_write(&o, &b, shown);
    fr_toml_table(&o, "theme");
    fr_toml_string(&o, "d", "#ffffff");
    CHECK(strstr(o.v, "[bookmarks]\n") != NULL);
    CHECK(strstr(o.v, "d = \"/tmp\"\n") != NULL);
    CHECK(strstr(o.v, "1 = \"sftp://nas/srv\"\n") != NULL);
    fr_toml t = fr_toml_parse(o.v);
    CHECK(t.error_line == 0);
    fr_bookmarks back = {0};
    fr_bookmark_set(&back, 'z', "file:///stale");
    fr_bookmarks_parse(&back, &t, to_uri);
    CHECK(fr_bookmarks_equal(&back, &b));
    fr_toml_free(&t);
    free(o.v);

    fr_bookmarks copy = fr_bookmarks_copy(&b);
    CHECK(fr_bookmarks_equal(&copy, &b));
    CHECK(fr_bookmark_remove(&copy, 'D') == 1);
    CHECK(fr_bookmark_remove(&copy, 'D') == 0);
    CHECK(!fr_bookmarks_equal(&copy, &b));
    CHECK(copy.n == 2 && copy.v[0].key == '1' && copy.v[1].key == 'd');
    fr_bookmarks_free(&copy);

    /* by hand: keys that are not one letter or digit, values that are not strings or locations, are skipped */
    t = fr_toml_parse("[bookmarks]\n"
                      "a = \"/srv/a\"\n"
                      "ab = \"/srv/ab\"\n"
                      "\"%\" = \"/srv/pct\"\n"
                      "b = true\n"
                      "c = \"relative\"\n"
                      "a = \"/srv/a2\"\n"
                      "[other]\n"
                      "e = \"/srv/e\"\n");
    fr_bookmarks_parse(&back, &t, to_uri);
    CHECK(back.n == 1);
    CHECK_STR(fr_bookmark_get(&back, 'a'), "file:///srv/a2");
    fr_toml_free(&t);
    fr_bookmarks_free(&back);
    fr_bookmarks_free(&b);
    return CHECK_DONE();
}

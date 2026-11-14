#include "check.h"
#include "core/toml.h"

int main(void)
{
    fr_strs favs = {0};
    FR_PUSH(favs, fr_strdup("/home/a b/Documents"));
    FR_PUSH(favs, fr_strdup("smb://nas/share \"x\"\\y"));
    FR_PUSH(favs, fr_strdup("tab\there"));

    fr_toml_out o = {0};
    fr_toml_comment(&o, "a comment");
    fr_toml_string(&o, "layout", "grid");
    fr_toml_bool(&o, "reverse", 1);
    fr_toml_bool(&o, "show_hidden", 0);
    fr_toml_strings(&o, "favorites", &favs);
    fr_toml_strings(&o, "empty", &(fr_strs){0});
    fr_toml_comment(&o, "");
    fr_toml_table(&o, "theme");
    fr_toml_string(&o, "accent", "#88c0d0");

    fr_toml t = fr_toml_parse(o.v);
    CHECK(t.error_line == 0);
    const fr_toml_value *v = fr_toml_get(&t, "", "layout", TOML_STRING);
    CHECK(v && !strcmp(v->str, "grid"));
    v = fr_toml_get(&t, "", "reverse", TOML_BOOL);
    CHECK(v && v->b == 1);
    v = fr_toml_get(&t, "", "show_hidden", TOML_BOOL);
    CHECK(v && v->b == 0);
    v = fr_toml_get(&t, "", "favorites", TOML_STRINGS);
    CHECK(v && v->strs.n == 3);
    for (int i = 0; v && i < v->strs.n && i < favs.n; i++) CHECK_STR(v->strs.v[i], favs.v[i]);
    v = fr_toml_get(&t, "", "empty", TOML_STRINGS);
    CHECK(v && v->strs.n == 0);
    v = fr_toml_get(&t, "theme", "accent", TOML_STRING);
    CHECK(v && !strcmp(v->str, "#88c0d0"));
    CHECK(fr_toml_get(&t, "", "accent", TOML_STRING) == NULL);
    CHECK(fr_toml_get(&t, "", "layout", TOML_BOOL) == NULL);
    fr_toml_free(&t);
    free(o.v);
    fr_strs_free(&favs);

    t = fr_toml_parse("# hand written\r\n"
                      "sort = 'size'   # literal string\n"
                      "\"quoted key\" = \"caf\\u00e9\"\n"
                      "number = 3\n"
                      "favorites = [ # a list\n"
                      "  \"/a\",\n"
                      "\n"
                      "  '/b' # last, no comma\n"
                      "]\n"
                      "sort = \"name\"\n"
                      "[ theme ]\n"
                      "red = \"#ff0000\"\n");
    CHECK(t.error_line == 4);
    v = fr_toml_get(&t, "", "sort", TOML_STRING);
    CHECK(v && !strcmp(v->str, "name"));
    v = fr_toml_get(&t, "", "quoted key", TOML_STRING);
    CHECK(v && !strcmp(v->str, "caf\xc3\xa9"));
    CHECK(fr_toml_get(&t, "", "number", TOML_STRING) == NULL);
    v = fr_toml_get(&t, "", "favorites", TOML_STRINGS);
    CHECK(v && v->strs.n == 2 && !strcmp(v->strs.v[0], "/a") && !strcmp(v->strs.v[1], "/b"));
    v = fr_toml_get(&t, "theme", "red", TOML_STRING);
    CHECK(v && !strcmp(v->str, "#ff0000"));
    fr_toml_free(&t);

    t = fr_toml_parse("a = \"unterminated\nb = true\nc = [\"x\" \"y\"]\n");
    CHECK(t.error_line == 1);
    v = fr_toml_get(&t, "", "b", TOML_BOOL);
    CHECK(v && v->b == 1);
    CHECK(fr_toml_get(&t, "", "c", TOML_STRINGS) == NULL);
    fr_toml_free(&t);
    return CHECK_DONE();
}

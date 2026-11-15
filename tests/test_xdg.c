#include "check.h"
#include "core/xdg.h"

static fr_strs strs(int n, ...);

static void check_argv(const fr_strs *a, int n, const char *const *want)
{
    CHECK(a->n == n);
    for (int i = 0; i < a->n && i < n; i++) CHECK_STR(a->v[i], want[i]);
}

int main(void)
{
    /* ---- desktop entries ---- */
    const char *desktop = "# a comment\n"
                          "[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=Text Editor\n"
                          "Name[de]=Texteditor\n"
                          "Exec=gedit --new-window %U\n"
                          "Icon = org.gnome.gedit\r\n"
                          "MimeType=text/plain;application/x-zerosize;text/x-c\\;weird;\n"
                          "Terminal=false\n"
                          "NoDisplay=true\n"
                          "[Desktop Action new]\n"
                          "Name=New Window\n"
                          "Exec=gedit --other\n";
    fr_desktop_entry e;
    CHECK(fr_desktop_parse(desktop, &e) == 0);
    CHECK_STR(e.name, "Text Editor");
    CHECK_STR(e.exec, "gedit --new-window %U");
    CHECK_STR(e.icon, "org.gnome.gedit");
    CHECK(e.application && !e.terminal && e.no_display && !e.hidden && !e.try_exec);
    CHECK(e.mime_types.n == 3);
    if (e.mime_types.n == 3) {
        CHECK_STR(e.mime_types.v[0], "text/plain");
        CHECK_STR(e.mime_types.v[2], "text/x-c;weird");
    }
    fr_desktop_entry_free(&e);
    CHECK(fr_desktop_parse("[Other]\nName=x\n", &e) == -1);
    fr_desktop_entry_free(&e);
    CHECK(fr_desktop_parse("[Desktop Entry]\nType=Link\nExec=a\\sb\\\\c\nTerminal=true\n", &e) == 0);
    CHECK(!e.application && e.terminal);
    CHECK_STR(e.exec, "a b\\c");
    fr_desktop_entry_free(&e);

    /* ---- Exec splitting ---- */
    fr_strs a = {0};
    CHECK(fr_exec_split("vim  -p\t\"my file\" \"a \\\"q\\\" \\$HOME \\\\ \\`x\\`\" ''", &a) == 0);
    check_argv(&a, 5, (const char *[]){"vim", "-p", "my file", "a \"q\" $HOME \\ `x`", ""});
    fr_strs_free(&a);
    CHECK(fr_exec_split("prog \"open quote", &a) == -1);
    fr_strs_free(&a);
    CHECK(fr_exec_split("sh -c 'echo hi'", &a) == 0);
    check_argv(&a, 3, (const char *[]){"sh", "-c", "echo hi"});
    fr_strs_free(&a);

    /* ---- Exec field codes ---- */
    fr_strs two = strs(2, "/tmp/a b.txt", "/tmp/c.txt"), none = {0};
    fr_argvs out = {0};
    CHECK(fr_exec_expand("gedit --new-window %U", &two, "Gedit", "ic", "/x.desktop", &out) == 0);
    CHECK(out.n == 1);
    if (out.n == 1) check_argv(&out.v[0], 4, (const char *[]){"gedit", "--new-window", "/tmp/a b.txt", "/tmp/c.txt"});
    fr_argvs_free(&out);
    CHECK(fr_exec_expand("mpv --title=%c %i -- %f", &two, "Player", "mpv", NULL, &out) == 0);
    CHECK(out.n == 2);
    if (out.n == 2) {
        check_argv(&out.v[0], 6, (const char *[]){"mpv", "--title=Player", "--icon", "mpv", "--", "/tmp/a b.txt"});
        check_argv(&out.v[1], 6, (const char *[]){"mpv", "--title=Player", "--icon", "mpv", "--", "/tmp/c.txt"});
    }
    fr_argvs_free(&out);
    CHECK(fr_exec_expand("app %d %D %n %N %v %m 100%% %k", &two, "A", NULL, "/usr/share/applications/app.desktop", &out) == 0);
    CHECK(out.n == 2);
    if (out.n == 2) check_argv(&out.v[0], 4, (const char *[]){"app", "100%", "/usr/share/applications/app.desktop", "/tmp/a b.txt"});
    fr_argvs_free(&out);
    CHECK(fr_exec_expand("viewer %f", &none, "V", NULL, NULL, &out) == 0);
    CHECK(out.n == 1);
    if (out.n == 1) check_argv(&out.v[0], 1, (const char *[]){"viewer"});
    fr_argvs_free(&out);
    CHECK(fr_exec_expand("%f", &none, "V", NULL, NULL, &out) == -1);
    CHECK(fr_exec_expand("\"unclosed %f", &two, "V", NULL, NULL, &out) == -1);
    CHECK(fr_exec_expand("", &two, "V", NULL, NULL, &out) == -1);
    fr_strs_free(&two);

    /* ---- mimeapps.list ---- */
    const char *apps = "[Added Associations]\n"
                       "image/png=gimp.desktop;\n"
                       "\n"
                       "[Default Applications]\n"
                       "text/plain=nvim.desktop;gedit.desktop\n"
                       "image/png = eog.desktop;\n"
                       "\n"
                       "[Removed Associations]\n"
                       "text/plain=nano.desktop\n";
    fr_strs ids = {0};
    fr_mimeapps_get(apps, "Default Applications", "text/plain", &ids);
    check_argv(&ids, 2, (const char *[]){"nvim.desktop", "gedit.desktop"});
    fr_strs_free(&ids);
    fr_mimeapps_get(apps, "Default Applications", "image/png", &ids);
    check_argv(&ids, 1, (const char *[]){"eog.desktop"});
    fr_strs_free(&ids);
    fr_mimeapps_get(apps, "Added Associations", "image/png", &ids);
    check_argv(&ids, 1, (const char *[]){"gimp.desktop"});
    fr_strs_free(&ids);
    char *t = fr_mimeapps_set_default(apps, "image/png", "gimp.desktop");
    CHECK_STR(t, "[Added Associations]\nimage/png=gimp.desktop;\n\n[Default Applications]\n"
                 "text/plain=nvim.desktop;gedit.desktop\nimage/png=gimp.desktop\n\n[Removed Associations]\ntext/plain=nano.desktop\n");
    free(t);
    t = fr_mimeapps_set_default(apps, "video/mp4", "mpv.desktop");
    CHECK_STR(t, "[Added Associations]\nimage/png=gimp.desktop;\n\n[Default Applications]\n"
                 "text/plain=nvim.desktop;gedit.desktop\nimage/png = eog.desktop;\nvideo/mp4=mpv.desktop\n\n"
                 "[Removed Associations]\ntext/plain=nano.desktop\n");
    free(t);
    t = fr_mimeapps_set_default("[Added Associations]\nx/y=a.desktop", "video/mp4", "mpv.desktop");
    CHECK_STR(t, "[Added Associations]\nx/y=a.desktop\n\n[Default Applications]\nvideo/mp4=mpv.desktop\n");
    free(t);
    t = fr_mimeapps_set_default(NULL, "video/mp4", "mpv.desktop");
    CHECK_STR(t, "[Default Applications]\nvideo/mp4=mpv.desktop\n");
    free(t);
    t = fr_mimeapps_set_default("[Default Applications]", "a/b", "c.desktop");
    CHECK_STR(t, "[Default Applications]\na/b=c.desktop\n");
    free(t);

    /* ---- the MIME database ---- */
    const char *aliases = "application/x-yaml application/yaml\ntext/x-c text/x-csrc\n";
    const char *subclasses = "text/x-csrc text/plain\napplication/x-shellscript application/x-executable\n"
                             "application/x-shellscript text/plain\napplication/yaml text/plain\n";
    t = fr_mime_unalias(aliases, "text/x-c");
    CHECK_STR(t, "text/x-csrc");
    free(t);
    t = fr_mime_unalias(aliases, "image/png");
    CHECK_STR(t, "image/png");
    free(t);
    fr_mime_ancestors(subclasses, "application/x-shellscript", &ids);
    check_argv(&ids, 3, (const char *[]){"application/x-shellscript", "application/x-executable", "text/plain"});
    fr_strs_free(&ids);
    fr_mime_ancestors(subclasses, "text/markdown", &ids);
    check_argv(&ids, 2, (const char *[]){"text/markdown", "text/plain"});
    fr_strs_free(&ids);
    fr_mime_ancestors(NULL, "image/png", &ids);
    check_argv(&ids, 1, (const char *[]){"image/png"});
    fr_strs_free(&ids);
    return CHECK_DONE();
}

#include <stdarg.h>
static fr_strs strs(int n, ...)
{
    fr_strs s = {0};
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) FR_PUSH(s, fr_strdup(va_arg(ap, const char *)));
    va_end(ap);
    return s;
}

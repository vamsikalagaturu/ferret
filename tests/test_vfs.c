#define _GNU_SOURCE
#include "check.h"
#include "vfs/vfs.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

static void write_file(const char *uri, const char *text)
{
    int err = 0;
    fr_stream *s = fr_vfs_open_write(uri, 0, &err);
    CHECK(s != NULL);
    if (!s) return;
    CHECK(s->write(s, text, strlen(text)) == (long)strlen(text));
    CHECK(s->close(s) == 0);
}

int main(void)
{
    char *p = fr_uri_parent("file:///home/a/b");
    CHECK_STR(p, "file:///home/a");
    free(p);
    p = fr_uri_parent("file:///home");
    CHECK_STR(p, "file:///");
    free(p);
    CHECK(fr_uri_parent("file:///") == NULL);
    p = fr_uri_parent("smb://nas/share/dir");
    CHECK_STR(p, "smb://nas/share");
    free(p);
    p = fr_uri_name("file:///home/a b.txt");
    CHECK_STR(p, "a b.txt");
    free(p);
    p = fr_uri_name("file:///");
    CHECK_STR(p, "");
    free(p);
    p = fr_uri_join("file:///", "etc");
    CHECK_STR(p, "file:///etc");
    free(p);
    CHECK_STR(fr_uri_path("file:///x/y"), "/x/y");
    CHECK_STR(fr_uri_path("smb://nas"), "/");
    CHECK(fr_uri_local("smb://nas/x") == NULL);
    CHECK(fr_uri_within("file:///a", "file:///a/b"));
    CHECK(fr_uri_within("file:///a", "file:///a"));
    CHECK(!fr_uri_within("file:///a", "file:///ab"));
    CHECK(fr_uri_within("file:///", "file:///ab"));
    CHECK(fr_uri_same_backend("file:///a", "file:///b"));
    CHECK(!fr_uri_same_backend("smb://x/a", "smb://y/a"));
    CHECK(fr_name_valid("a.txt") && !fr_name_valid("..") && !fr_name_valid("a/b") && !fr_name_valid(""));

    char tmpl[] = "/tmp/ferret-test-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    char *root = fr_uri_from_path(tmpl);
    char *dir = fr_uri_join(root, "dir"), *file = fr_uri_join(root, "a.txt"), *hidden = fr_uri_join(root, ".h");
    CHECK(fr_vfs_mkdir(dir) == 0);
    CHECK(fr_vfs_mkdir(dir) == -EEXIST);
    write_file(file, "hello");
    write_file(hidden, "");

    fr_entries es = {0};
    CHECK(fr_vfs_list(root, &es) == 0);
    CHECK(es.n == 3);
    for (int i = 0; i < es.n; i++) {
        if (!strcmp(es.v[i].name, "dir")) CHECK(es.v[i].is_dir);
        if (!strcmp(es.v[i].name, "a.txt")) CHECK(!es.v[i].is_dir && es.v[i].size == 5);
        if (!strcmp(es.v[i].name, ".h")) CHECK(es.v[i].hidden);
    }
    fr_entries_free(&es);

    char *taken = fr_uri_free_name(root, "a.txt", 0), *want = fr_format("%s/a (2).txt", root);
    CHECK_STR(taken, want);
    free(taken), free(want);

    char *b = fr_uri_join(root, "b.txt");
    CHECK(fr_vfs_rename(file, b) == 0);
    CHECK(!fr_vfs_exists(file) && fr_vfs_exists(b));
    write_file(file, "x");
    CHECK(fr_vfs_rename(file, b) == -EEXIST);

    int err = 0;
    fr_stream *s = fr_vfs_open_write(b, 1, &err);
    CHECK(!s && err == -EEXIST);
    s = fr_vfs_open_read(b, &err);
    char buf[16] = {0};
    CHECK(s && s->read(s, buf, sizeof buf) == 5 && !strcmp(buf, "hello"));
    if (s) s->close(s);

    CHECK(fr_vfs_remove(dir) == 0);
    CHECK(fr_vfs_remove(b) == 0);
    CHECK(fr_vfs_remove(file) == 0);
    CHECK(fr_vfs_remove(hidden) == 0);
    CHECK(fr_vfs_remove(root) == 0);
    CHECK(fr_vfs_list("nope://x", &es) == -EPROTONOSUPPORT);
    free(root), free(dir), free(file), free(hidden), free(b);
    return CHECK_DONE();
}

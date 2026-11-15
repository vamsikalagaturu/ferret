/* src/core/recent.c: the recent folders' order and cap, and no passwords kept */
#include "check.h"
#include "core/recent.h"

static void strips(const char *uri, const char *want)
{
    char *s = fr_uri_strip_password(uri);
    CHECK_STR(s, want);
    free(s);
}

static void test_strip(void)
{
    strips("file:///home/u/x", "file:///home/u/x");
    strips("sftp://bob@host:22/srv", "sftp://bob@host:22/srv");
    strips("sftp://bob:secret@host:22/srv", "sftp://bob@host:22/srv");
    strips("smb://bob:p%40ss@nas/media/a@b", "smb://bob@nas/media/a@b");
    strips("ftp://host:21/a:b@c", "ftp://host:21/a:b@c");
    /* an archive on a server: the outer URI's password goes too */
    strips("archive://sftp://bob:pw@host/x.zip!/docs", "archive://sftp://bob@host/x.zip!/docs");
    strips("trash:///", "trash:///");
}

static void test_push(void)
{
    fr_strs l = {0};
    fr_recent_push(&l, "file:///a", 3);
    fr_recent_push(&l, "file:///b", 3);
    fr_recent_push(&l, "file:///c", 3);
    CHECK(l.n == 3);
    CHECK_STR(l.v[0], "file:///c");
    CHECK_STR(l.v[2], "file:///a");
    /* again: to the front, not twice */
    fr_recent_push(&l, "file:///a", 3);
    CHECK(l.n == 3);
    CHECK_STR(l.v[0], "file:///a");
    CHECK_STR(l.v[1], "file:///c");
    CHECK_STR(l.v[2], "file:///b");
    /* past the cap the oldest goes */
    fr_recent_push(&l, "file:///d", 3);
    CHECK(l.n == 3);
    CHECK_STR(l.v[0], "file:///d");
    CHECK_STR(l.v[2], "file:///c");
    /* the same server folder with and without the password is one entry */
    fr_recent_push(&l, "sftp://bob:pw@h/x", 3);
    fr_recent_push(&l, "sftp://bob@h/x", 3);
    CHECK(l.n == 3);
    CHECK_STR(l.v[0], "sftp://bob@h/x");
    CHECK_STR(l.v[1], "file:///d");
    fr_strs_free(&l);
    for (int i = 0; i < 30; i++) {
        char *u = fr_format("file:///f%d", i);
        fr_recent_push(&l, u, FR_RECENT_MAX);
        free(u);
    }
    CHECK(l.n == FR_RECENT_MAX);
    CHECK_STR(l.v[0], "file:///f29");
    CHECK_STR(l.v[FR_RECENT_MAX - 1], "file:///f10");
    fr_strs_free(&l);
}

int main(void)
{
    test_strip();
    test_push();
    return CHECK_DONE();
}

#define _GNU_SOURCE
#include "check.h"
#include "jobs/jobs.h"
#include "jobs/rename.h"
#include "vfs/vfs.h"
#include <stdlib.h>
#include <unistd.h>

static char *root;

static char *at(const char *rel) { return fr_format("%s/%s", root, rel); }

static void put(const char *rel, const char *text)
{
    char *uri = at(rel);
    int err = 0;
    fr_stream *s = fr_vfs_open_write(uri, 0, &err);
    CHECK(s != NULL);
    if (s) s->write(s, text, strlen(text)), s->close(s);
    free(uri);
}

static char *get(const char *rel)
{
    char *uri = at(rel);
    int err = 0;
    fr_stream *s = fr_vfs_open_read(uri, &err);
    free(uri);
    if (!s) return NULL;
    char buf[64];
    long n = s->read(s, buf, sizeof buf - 1);
    s->close(s);
    return fr_strndup(buf, n > 0 ? (size_t)n : 0);
}

#define CHECK_FILE(rel, text)                                                                                          \
    do {                                                                                                               \
        char *t_ = get(rel);                                                                                           \
        CHECK_STR(t_, text);                                                                                           \
        free(t_);                                                                                                      \
    } while (0)

static void rule_is(const char *find, const char *replace, int regex, const char *name, int n, const char *want)
{
    fr_rename_rule r = {.find = find, .replace = replace, .regex = regex};
    char err[256];
    CHECK(fr_rename_rule_compile(&r, err, sizeof err) == 0);
    char *got = fr_rename_rule_apply(&r, name, 0, n);
    CHECK_STR(got, want);
    free(got);
    fr_rename_rule_free(&r);
}

/* plans from[i] -> to[i] (names in root) and runs it as a job; the plan's result */
static int rename_all(const char **from, const char **to, int n, int *steps)
{
    fr_strs f = {0}, t = {0};
    for (int i = 0; i < n; i++) {
        FR_PUSH(f, at(from[i]));
        FR_PUSH(t, at(to[i]));
    }
    fr_steps plan;
    char err[256] = "";
    int rc = fr_rename_plan(&f, &t, &plan, err, sizeof err);
    if (steps) *steps = plan.n;
    if (!rc) {
        int id = fr_job_submit_steps(&plan, "Renaming");
        fr_jobs_wait_idle();
        fr_job_status st[4];
        int k = fr_jobs_list(st, 4);
        for (int i = 0; i < k; i++)
            if (st[i].id == id) CHECK(st[i].failed == 0);
        fr_job_dismiss(id);
    }
    fr_steps_free(&plan);
    fr_strs_free(&f), fr_strs_free(&t);
    return rc;
}

int main(void)
{
    /* rules */
    rule_is("", "photo-{n:3}", 0, "IMG_1234.JPG", 7, "photo-007.JPG");
    rule_is("", "", 0, "a.txt", 1, "a.txt");
    rule_is("IMG_", "", 0, "IMG_1.jpg", 1, "1.jpg");
    rule_is("a", "b", 0, "banana", 1, "bbnbnb");
    rule_is("([0-9]+)", "<\\1>", 1, "a12b3", 1, "a<12>b<3>");
    rule_is("^(.*)-(.*)$", "\\2-\\1", 1, "left-right", 1, "right-left");
    rule_is("x*", "-", 1, "ab", 1, "-a-b");
    rule_is("^", "{n:2}_", 1, "song.mp3", 3, "03_song.mp3");
    rule_is("\\.jpeg$", ".jpg", 1, "a.jpeg", 1, "a.jpg");
    fr_rename_rule bad = {.find = "(", .replace = "", .regex = 1};
    char err[256];
    CHECK(fr_rename_rule_compile(&bad, err, sizeof err) == -1 && *err);

    char tmpl[] = "/tmp/ferret-rename-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    root = fr_uri_from_path(tmpl);
    fr_jobs_start(NULL);
    put("a", "A");
    put("b", "B");
    put("c", "C");
    put("keep", "K");

    /* a swap goes through a temporary name: three steps */
    int steps = 0;
    CHECK(rename_all((const char *[]){"a", "b"}, (const char *[]){"b", "a"}, 2, &steps) == 0);
    CHECK(steps == 3);
    CHECK_FILE("a", "B");
    CHECK_FILE("b", "A");

    /* a rotation of three, and a chain into a free name, which needs no temporary one */
    CHECK(rename_all((const char *[]){"a", "b", "c"}, (const char *[]){"b", "c", "a"}, 3, &steps) == 0);
    CHECK(steps == 4);
    CHECK_FILE("b", "B");
    CHECK_FILE("c", "A");
    CHECK_FILE("a", "C");
    CHECK(rename_all((const char *[]){"a", "b"}, (const char *[]){"b", "z"}, 2, &steps) == 0);
    CHECK(steps == 2);
    CHECK_FILE("z", "B");
    CHECK_FILE("b", "C");

    /* conflicts: two items onto one name, a name taken by an item not renamed, an invalid name */
    CHECK(rename_all((const char *[]){"b", "c"}, (const char *[]){"x", "x"}, 2, NULL) == -1);
    CHECK(rename_all((const char *[]){"b"}, (const char *[]){"keep"}, 1, NULL) == -1);
    CHECK(rename_all((const char *[]){"b"}, (const char *[]){"."}, 1, NULL) == -1);
    fr_strs f = {0}, t = {0};
    fr_steps plan;
    FR_PUSH(f, at("b"));
    FR_PUSH(t, at("b"));
    CHECK(fr_rename_plan(&f, &t, &plan, err, sizeof err) == 0 && plan.n == 0);
    fr_strs_free(&f), fr_strs_free(&t);
    CHECK_FILE("b", "C");
    CHECK_FILE("keep", "K");

    fr_jobs_stop();
    free(root);
    return CHECK_DONE();
}

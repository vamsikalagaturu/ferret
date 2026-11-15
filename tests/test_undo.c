#define _GNU_SOURCE
#include "check.h"
#include "jobs/jobs.h"
#include "jobs/rename.h"
#include "vfs/vfs.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char *root, *dir;
static fr_history h;

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

static int exists(const char *rel)
{
    char *uri = at(rel);
    int r = fr_vfs_exists(uri);
    free(uri);
    return r;
}

static unsigned mode_of(const char *rel)
{
    char *p = fr_format("%s/%s", dir, rel);
    struct stat st;
    unsigned m = stat(p, &st) ? 0 : (unsigned)(st.st_mode & 07777);
    free(p);
    return m;
}

#define CHECK_FILE(rel, text)                                                                                          \
    do {                                                                                                               \
        char *t_ = get(rel);                                                                                           \
        CHECK_STR(t_, text);                                                                                           \
        free(t_);                                                                                                      \
    } while (0)

/* waits for job id and records what it did, as the window does */
static void record(int id, const char *label, int failures)
{
    fr_jobs_wait_idle();
    fr_job_status st[8];
    int k = fr_jobs_list(st, 8);
    for (int i = 0; i < k; i++)
        if (st[i].id == id) CHECK(st[i].failed == failures);
    fr_op op = {.label = fr_strdup(label)};
    CHECK(fr_job_take_log(id, &op.steps) == 0);
    fr_job_dismiss(id);
    fr_history_push(&h, op);
}

static int job(fr_job_kind kind, const char *rel, const char *dest_rel)
{
    fr_strs src = {0};
    FR_PUSH(src, at(rel));
    char *dest = dest_rel ? at(dest_rel) : NULL;
    int id = fr_job_submit(kind, &src, dest);
    fr_strs_free(&src);
    free(dest);
    return id;
}

/* undo (redo 0) or redo (redo 1) the latest: its steps turned around, run, and what that did kept for the other way */
static int step_back(int redo)
{
    fr_op op;
    if (fr_history_pop(&h, redo, &op)) return -1;
    fr_steps inv = fr_steps_inverse(&op.steps);
    int id = fr_job_submit_steps(&inv, redo ? "Redoing" : "Undoing");
    fr_steps_free(&inv);
    fr_jobs_wait_idle();
    fr_job_status st[8];
    int k = fr_jobs_list(st, 8), failed = 0;
    for (int i = 0; i < k; i++)
        if (st[i].id == id) failed = st[i].failed;
    fr_steps_free(&op.steps);
    CHECK(fr_job_take_log(id, &op.steps) == 0);
    fr_job_dismiss(id);
    fr_history_put(&h, !redo, op);
    return failed;
}

static void check_before(void)
{
    CHECK_FILE("src/a.txt", "alpha");
    CHECK(!exists("out/a.txt"));
    CHECK_FILE("src/b.txt", "beta");
    CHECK(!exists("out/b.txt"));
    CHECK_FILE("src/c.txt", "gamma");
    CHECK(mode_of("src/d.txt") == 0644);
    CHECK(!exists("out/new"));
    CHECK_FILE("src/x", "X");
    CHECK_FILE("src/y", "Y");
}

static void check_after(void)
{
    CHECK_FILE("src/a.txt", "alpha");
    CHECK_FILE("out/a.txt", "alpha");
    CHECK(!exists("src/b.txt"));
    CHECK_FILE("out/b.txt", "beta");
    CHECK(!exists("src/c.txt"));
    CHECK(mode_of("src/d.txt") == 0600);
    CHECK(exists("out/new"));
    CHECK_FILE("src/x", "Y");
    CHECK_FILE("src/y", "X");
}

int main(void)
{
    char tmpl[] = "/tmp/ferret-undo-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    dir = tmpl;
    root = fr_uri_from_path(tmpl);
    /* the trash on the same device, inside the test's folder */
    char *data = fr_format("%s/data", tmpl);
    setenv("XDG_DATA_HOME", data, 1);
    fr_jobs_start(NULL);
    char *u = at("src");
    CHECK(fr_vfs_mkdir(u) == 0);
    free(u);
    u = at("out");
    CHECK(fr_vfs_mkdir(u) == 0);
    free(u);
    put("src/a.txt", "alpha");
    put("src/b.txt", "beta");
    put("src/c.txt", "gamma");
    put("src/d.txt", "delta");
    put("src/x", "X");
    put("src/y", "Y");
    char *d = fr_format("%s/src/d.txt", tmpl);
    chmod(d, 0644);
    free(d);
    check_before();

    record(job(JOB_COPY, "src/a.txt", "out"), "copied a.txt", 0);
    record(job(JOB_MOVE, "src/b.txt", "out"), "moved b.txt", 0);
    record(job(JOB_TRASH, "src/c.txt", NULL), "trashed c.txt", 0);
    fr_strs src = {0};
    FR_PUSH(src, at("src/d.txt"));
    record(fr_job_submit_chmod(&src, 0, 0044), "chmod d.txt", 0);
    fr_strs_free(&src);
    /* a folder made in the window: its record is made there, by hand */
    u = at("out/new");
    CHECK(fr_vfs_mkdir(u) == 0);
    fr_op made = {.label = fr_strdup("made new")};
    fr_steps_add(&made.steps, STEP_CREATED, u, NULL);
    made.steps.v[0].is_dir = 1;
    fr_history_push(&h, made);
    free(u);
    fr_strs f = {0}, t = {0};
    FR_PUSH(f, at("src/x"));
    FR_PUSH(f, at("src/y"));
    FR_PUSH(t, at("src/y"));
    FR_PUSH(t, at("src/x"));
    fr_steps plan;
    char err[256];
    CHECK(fr_rename_plan(&f, &t, &plan, err, sizeof err) == 0);
    record(fr_job_submit_steps(&plan, "Renaming"), "swapped x and y", 0);
    fr_steps_free(&plan);
    fr_strs_free(&f), fr_strs_free(&t);
    check_after();
    CHECK(h.undo.n == 6 && h.redo.n == 0);

    /* every one taken back, newest first */
    for (int i = 0; i < 6; i++) CHECK(step_back(0) == 0);
    CHECK(step_back(0) == -1);
    check_before();
    CHECK(h.undo.n == 0 && h.redo.n == 6);
    /* the trash's record of c.txt went with it; the copy of a.txt is in the trash now */
    char *info = fr_format("%s/Trash/info/c.txt.trashinfo", data), *copy = fr_format("%s/Trash/files/a.txt", data);
    CHECK(access(info, F_OK) != 0);
    CHECK(access(copy, F_OK) == 0);

    /* and done again, oldest first */
    for (int i = 0; i < 6; i++) CHECK(step_back(1) == 0);
    CHECK(step_back(1) == -1);
    check_after();
    CHECK(access(copy, F_OK) != 0);

    /* undo once more, then a new operation forgets what could be redone */
    CHECK(step_back(0) == 0);
    CHECK_FILE("src/x", "X");
    CHECK(h.redo.n == 1);
    record(job(JOB_TRASH, "src/x", NULL), "trashed x", 0);
    CHECK(h.redo.n == 0 && h.undo.n == 6);

    /* putting back onto a name taken again fails and leaves both */
    put("src/x", "new X");
    CHECK(step_back(0) == 1);
    CHECK_FILE("src/x", "new X");

    /* a permanent delete leaves nothing to undo */
    int id = job(JOB_DELETE, "src/y", NULL);
    fr_jobs_wait_idle();
    fr_steps none;
    CHECK(fr_job_take_log(id, &none) == 0 && none.n == 0);
    fr_job_dismiss(id);

    fr_history_clear(&h);
    fr_jobs_stop();
    free(info), free(copy), free(data), free(root);
    return CHECK_DONE();
}

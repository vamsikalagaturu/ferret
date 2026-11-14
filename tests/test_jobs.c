#define _GNU_SOURCE
#include "check.h"
#include "jobs/jobs.h"
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
    char buf[256];
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

static void mk(const char *rel)
{
    char *uri = at(rel);
    CHECK(fr_vfs_mkdir(uri) == 0);
    free(uri);
}

static fr_job_status run(fr_job_kind kind, const char **rels, int n, const char *dest_rel)
{
    fr_strs src = {0};
    for (int i = 0; i < n; i++) FR_PUSH(src, at(rels[i]));
    char *dest = dest_rel ? at(dest_rel) : NULL;
    int id = fr_job_submit(kind, &src, dest);
    fr_jobs_wait_idle();
    fr_job_status st[8];
    int k = fr_jobs_list(st, 8);
    fr_job_status r = {0};
    for (int i = 0; i < k; i++)
        if (st[i].id == id) r = st[i];
    fr_job_dismiss(id);
    fr_strs_free(&src);
    free(dest);
    return r;
}

/* answers the first conflict with `answer` once it is asked */
static fr_job_status run_answering(fr_job_kind kind, const char *rel, const char *dest_rel, fr_conflict answer)
{
    fr_strs src = {0};
    FR_PUSH(src, at(rel));
    char *dest = at(dest_rel);
    int id = fr_job_submit(kind, &src, dest);
    fr_job_status st[8], r = {0};
    for (int tries = 0; tries < 2000; tries++) {
        int k = fr_jobs_list(st, 8);
        for (int i = 0; i < k; i++)
            if (st[i].id == id) r = st[i];
        if (r.state == JOB_ASKING) {
            fr_job_answer(id, answer, 0);
            break;
        }
        if (r.state == JOB_DONE) break;
        usleep(1000);
    }
    CHECK(r.state == JOB_ASKING);
    fr_jobs_wait_idle();
    int k = fr_jobs_list(st, 8);
    for (int i = 0; i < k; i++)
        if (st[i].id == id) r = st[i];
    fr_job_dismiss(id);
    fr_strs_free(&src);
    free(dest);
    return r;
}

int main(void)
{
    char tmpl[] = "/tmp/ferret-jobs-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    root = fr_uri_from_path(tmpl);
    fr_jobs_start(NULL);

    mk("src");
    mk("src/sub");
    put("src/a.txt", "alpha");
    put("src/sub/b.txt", "beta");
    mk("out");

    /* copy a tree */
    fr_job_status st = run(JOB_COPY, (const char *[]){"src"}, 1, "out");
    CHECK(st.state == JOB_DONE && st.failed == 0);
    CHECK(st.bytes_total == 9 && st.bytes_done == 9);
    char *t = get("out/src/sub/b.txt");
    CHECK_STR(t, "beta");
    free(t);

    /* pasting into the same folder keeps both */
    st = run(JOB_COPY, (const char *[]){"src/a.txt"}, 1, "src");
    CHECK(st.failed == 0 && exists("src/a (2).txt"));

    /* a folder into itself */
    st = run(JOB_COPY, (const char *[]){"src"}, 1, "src/sub");
    CHECK(st.failed == 1 && !exists("src/sub/src"));

    /* conflicts: skip, replace, keep both */
    put("out/a.txt", "old");
    st = run_answering(JOB_COPY, "src/a.txt", "out", CONFLICT_SKIP);
    t = get("out/a.txt");
    CHECK_STR(t, "old");
    free(t);
    st = run_answering(JOB_COPY, "src/a.txt", "out", CONFLICT_REPLACE);
    t = get("out/a.txt");
    CHECK_STR(t, "alpha");
    free(t);
    st = run_answering(JOB_COPY, "src/a.txt", "out", CONFLICT_KEEP_BOTH);
    CHECK(exists("out/a (2).txt"));

    /* replacing a folder merges it */
    put("out/src/only-here.txt", "x");
    put("src/sub/b.txt", "beta2");
    st = run_answering(JOB_COPY, "src", "out", CONFLICT_REPLACE);
    CHECK(exists("out/src/only-here.txt"));
    t = get("out/src/sub/b.txt");
    CHECK_STR(t, "beta2");
    free(t);

    /* move, then move over an existing folder (merge) */
    mk("moved");
    st = run(JOB_MOVE, (const char *[]){"src/a (2).txt"}, 1, "moved");
    CHECK(st.failed == 0 && exists("moved/a (2).txt") && !exists("src/a (2).txt"));
    mk("moved/src");
    put("moved/src/keep.txt", "k");
    st = run_answering(JOB_MOVE, "src", "moved", CONFLICT_REPLACE);
    CHECK(st.failed == 0 && !exists("src") && exists("moved/src/keep.txt") && exists("moved/src/sub/b.txt"));

    /* delete a tree */
    st = run(JOB_DELETE, (const char *[]){"out", "moved"}, 2, NULL);
    CHECK(st.failed == 0 && !exists("out") && !exists("moved"));
    CHECK(fr_jobs_finished() > 0);

    fr_jobs_stop();
    CHECK(fr_vfs_remove(root) == 0);
    return CHECK_DONE();
}

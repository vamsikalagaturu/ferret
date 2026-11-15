/* The SMB and SFTP backends against real servers, when the environment names them; skipped (77) otherwise:
 *   FR_TEST_SFTP=sftp://user@host:port/writable/folder  FR_TEST_SMB=smb://user@host:port/share  FR_TEST_PASSWORD=...
 * The servers must refuse a guest and the user's own SSH keys. HOME and XDG_CONFIG_HOME point at a scratch folder
 * for the run, so the user's ~/.ssh and ferret's known_hosts stay out of it. FR_TEST_RESTART, a command that
 * restarts both servers, checks that broken connections reconnect. tests/net/server.sh starts a container with
 * both and prints these. */
#define _GNU_SOURCE
#include "check.h"
#include "jobs/jobs.h"
#include "net/net.h"
#include "vfs/vfs.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

static const char *password;

static void put(const char *uri, const char *text, size_t n)
{
    int err = 0;
    fr_stream *s = fr_vfs_open_write(uri, 0, &err);
    CHECK(s != NULL);
    if (!s) return;
    CHECK(s->write(s, text, n) == (long)n);
    CHECK(s->close(s) == 0);
}

static char *get(const char *uri, size_t *n)
{
    int err = 0;
    fr_stream *s = fr_vfs_open_read(uri, &err);
    CHECK(s != NULL);
    if (!s) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = fr_realloc(NULL, cap);
    for (;;) {
        if (len == cap) buf = fr_realloc(buf, cap *= 2);
        long r = s->read(s, buf + len, cap - len);
        CHECK(r >= 0);
        if (r <= 0) break;
        len += (size_t)r;
    }
    CHECK(s->close(s) == 0);
    *n = len;
    return buf;
}

static int jobs_ok(void)
{
    fr_jobs_wait_idle();
    fr_job_status st[16];
    int n = fr_jobs_list(st, 16), failed = 0;
    for (int i = 0; i < n; i++) {
        if (st[i].failed) fprintf(stderr, "job %d: %s\n", st[i].id, st[i].error);
        failed += st[i].failed;
        fr_job_dismiss(st[i].id);
    }
    return !failed;
}

/* signs in as the servers want: identity first (SFTP), then refused, wrong and right passwords */
static void sign_in(const char *base, int ssh)
{
    char *key = fr_net_key_of(base);
    fr_net_uri u;
    CHECK(fr_net_parse(base, &u) == 0);
    fr_entries es = {0};
    int rc = fr_vfs_list(base, &es);
    if (ssh) {
        CHECK(rc == -FR_EUNTRUSTED);
        char *q = fr_trust_question(key);
        CHECK(q && strstr(q, "SHA256:"));
        free(q);
        CHECK(fr_trust_answer(key, 1));
        rc = fr_vfs_list(base, &es);
    }
    CHECK(rc == -FR_EAUTH); /* no guest, no keys */
    CHECK(!fr_creds_refused(key));
    fr_creds_put(key, u.user, "wrong", 0);
    CHECK(fr_vfs_list(base, &es) == -FR_EAUTH);
    CHECK(fr_creds_refused(key));
    fr_creds_put(key, u.user, password, 0);
    CHECK(fr_vfs_list(base, &es) == 0);
    fr_entries_free(&es);
    fr_net_uri_free(&u);
    free(key);
}

static void exercise(const char *base, int ssh)
{
    fprintf(stderr, "-- %s\n", base);
    sign_in(base, ssh);
    char *dir = fr_uri_join(base, "ferret-test");
    if (fr_vfs_exists(dir)) {
        fr_strs one = {0};
        FR_PUSH(one, fr_strdup(dir));
        fr_job_submit(JOB_DELETE, &one, NULL);
        fr_strs_free(&one);
        CHECK(jobs_ok());
    }
    CHECK(fr_vfs_mkdir(dir) == 0);
    CHECK(fr_vfs_mkdir(dir) == -EEXIST);
    char *a = fr_uri_join(dir, "a.txt"), *b = fr_uri_join(dir, "b.txt");
    put(a, "hello", 5);
    int err = 0;
    CHECK(fr_vfs_open_write(a, 1, &err) == NULL && err == -EEXIST);
    fr_entry e = {0};
    CHECK(fr_vfs_stat(a, &e) == 0 && !e.is_dir && e.size == 5);
    CHECK_STR(e.name, "a.txt");
    fr_entry_free(&e);
    CHECK(fr_vfs_stat(dir, &e) == 0 && e.is_dir);
    fr_entry_free(&e);
    CHECK(fr_vfs_rename(a, b) == 0);
    CHECK(!fr_vfs_exists(a) && fr_vfs_exists(b));
    put(a, "x", 1);
    CHECK(fr_vfs_rename(a, b) == -EEXIST);
    size_t n;
    char *text = get(b, &n);
    CHECK(text && n == 5 && !memcmp(text, "hello", 5));
    free(text);
    const fr_backend *be = fr_vfs_backend(base);
    if (be->set_mtime) {
        CHECK(be->set_mtime(b, 1000000000) == 0);
        CHECK(fr_vfs_stat(b, &e) == 0 && e.mtime == 1000000000);
        fr_entry_free(&e);
    }
    uint64_t fb = 0, tb = 0;
    if (be->free_space) CHECK(be->free_space(dir, &fb, &tb) == 0 && tb > 0);
    fr_entries es = {0};
    CHECK(fr_vfs_list(dir, &es) == 0 && es.n == 2);
    fr_entries_free(&es);

    /* a folder there and back through the jobs, with a file bigger than one read or write */
    char tmpl[] = "/tmp/ferret-remote-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    char *local = fr_uri_from_path(tmpl), *tree = fr_uri_join(local, "tree"), *sub = fr_uri_join(tree, "sub");
    char *big = fr_uri_join(sub, "big.bin"), *back = fr_uri_join(local, "back");
    CHECK(fr_vfs_mkdir(tree) == 0 && fr_vfs_mkdir(sub) == 0 && fr_vfs_mkdir(back) == 0);
    size_t bn = 3 * 1024 * 1024 + 17;
    char *data = fr_realloc(NULL, bn);
    for (size_t i = 0; i < bn; i++) data[i] = (char)(i * 31 + i / 4096);
    put(big, data, bn);
    fr_strs src = {0};
    FR_PUSH(src, fr_strdup(tree));
    fr_job_submit(JOB_COPY, &src, dir);
    CHECK(jobs_ok());
    char *remote_tree = fr_uri_join(dir, "tree");
    fr_strs_free(&src);
    FR_PUSH(src, fr_strdup(remote_tree));
    fr_job_submit(JOB_COPY, &src, back);
    CHECK(jobs_ok());
    char *round = fr_format("%s/tree/sub/big.bin", back);
    char *got = get(round, &n);
    CHECK(got && n == bn && !memcmp(got, data, bn));
    free(got);
    char *moved = fr_uri_join(dir, "moved");
    CHECK(fr_vfs_rename(remote_tree, moved) == 0);
    fr_strs_free(&src);
    FR_PUSH(src, fr_strdup(dir));
    fr_job_submit(JOB_DELETE, &src, NULL);
    CHECK(jobs_ok());
    CHECK(!fr_vfs_exists(dir));
    fr_strs_free(&src);
    FR_PUSH(src, fr_strdup(local));
    fr_job_submit(JOB_DELETE, &src, NULL);
    CHECK(jobs_ok());
    fr_strs_free(&src);
    free(data), free(round), free(moved), free(remote_tree);
    free(local), free(tree), free(sub), free(big), free(back);
    free(dir), free(a), free(b);
}

int main(void)
{
    const char *sftp = getenv("FR_TEST_SFTP"), *smb = getenv("FR_TEST_SMB");
    password = getenv("FR_TEST_PASSWORD");
    if ((!sftp && !smb) || !password) {
        fprintf(stderr, "skipped: FR_TEST_SFTP / FR_TEST_SMB and FR_TEST_PASSWORD are not set\n");
        return 77;
    }
    char home[] = "/tmp/ferret-home-XXXXXX";
    CHECK(mkdtemp(home) != NULL);
    char *config = fr_format("%s/.config", home);
    setenv("HOME", home, 1);
    setenv("XDG_CONFIG_HOME", config, 1);
    unsetenv("SSH_AUTH_SOCK");
    fr_jobs_start(NULL);
    if (sftp) exercise(sftp, 1);
    if (smb) {
        exercise(smb, 0);
        char *root = fr_uri_parent(smb);
        fr_entries es = {0};
        CHECK(root && fr_vfs_list(root, &es) == 0);
        char *share = fr_uri_name(smb);
        int found = 0;
        for (int i = 0; i < es.n; i++) found |= !strcmp(es.v[i].name, share) && es.v[i].is_dir;
        CHECK(found);
        fr_entries_free(&es);
        free(root), free(share);
    }
    /* the server goes away under the pooled connections: the next call finds them broken and connects again */
    const char *restart = getenv("FR_TEST_RESTART");
    if (restart) {
        fr_entries es = {0};
        CHECK(system(restart) == 0);
        if (sftp) CHECK(fr_vfs_list(sftp, &es) == 0);
        fr_entries_free(&es);
        int rc = smb ? fr_vfs_list(smb, &es) : 0;
        if (rc) fprintf(stderr, "after the restart: %s: %s\n", smb, fr_vfs_error(rc));
        CHECK(rc == 0);
        fr_entries_free(&es);
    }
    fr_jobs_stop();
    fr_conn_close_all();
    char *known = fr_format("%s/ferret/known_hosts", config);
    if (sftp) CHECK(access(known, R_OK) == 0);
    unlink(known);
    free(known);
    char *d = fr_format("%s/ferret", config);
    rmdir(d), rmdir(config), rmdir(home);
    free(d), free(config);
    return CHECK_DONE();
}

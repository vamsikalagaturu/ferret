#define _GNU_SOURCE
#include "check.h"
#include "core/opencache.h"
#include "jobs/jobs.h"
#include "vfs/vfs.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) fputs(text, f), fclose(f);
}

static char *get(const char *path)
{
    char buf[256] = "";
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return fr_strndup(buf, n);
}

static void set_mtime(const char *path, time_t t)
{
    struct timespec ts[2] = {{t, 0}, {t, 0}};
    CHECK(utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW) == 0);
}

static fr_job_status fetch(const char *uri, const char *path)
{
    char *copy = fr_uri_from_path(path);
    int id = fr_job_submit_fetch(uri, copy);
    fr_jobs_wait_idle();
    fr_job_status st[8], r = {0};
    int k = fr_jobs_list(st, 8);
    for (int i = 0; i < k; i++)
        if (st[i].id == id) r = st[i];
    CHECK(r.kind == JOB_FETCH);
    fr_steps log = {0};
    CHECK(fr_job_take_log(id, &log) == 0 && log.n == 0);
    fr_steps_free(&log);
    fr_job_dismiss(id);
    free(copy);
    return r;
}

static void keying(void)
{
    char *a = fr_opencache_path("/c", "sftp://bob@host:22/docs/report.pdf", "report.pdf");
    char *b = fr_opencache_path("/c", "sftp://bob@host:22/docs/report.pdf", "report.pdf");
    char *c = fr_opencache_path("/c", "sftp://bob@host:22/old/report.pdf", "report.pdf");
    char *d = fr_opencache_path("/c", "archive://file:///a.zip!/x", "");
    CHECK_STR(a, b);
    CHECK(strcmp(a, c) != 0);
    /* root, 16 hex digits, the name */
    CHECK(strlen(a) == strlen("/c/") + 16 + strlen("/report.pdf"));
    CHECK(!strncmp(a, "/c/", 3) && a[19] == '/' && !strcmp(a + 20, "report.pdf"));
    for (int i = 3; i < 19; i++) CHECK(strchr("0123456789abcdef", a[i]) != NULL);
    CHECK(!strcmp(d + strlen(d) - 5, "/file"));
    free(a), free(b), free(c), free(d);

    const char *xdg = getenv("XDG_CACHE_HOME");
    char *saved = xdg ? fr_strdup(xdg) : NULL;
    setenv("XDG_CACHE_HOME", "/x/cache", 1);
    char *root = fr_opencache_root();
    CHECK_STR(root, "/x/cache/ferret/open");
    free(root);
    if (saved) setenv("XDG_CACHE_HOME", saved, 1);
    else unsetenv("XDG_CACHE_HOME");
    free(saved);
}

int main(void)
{
    keying();

    char tmpl[] = "/tmp/ferret-opencache-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    char *src = fr_format("%s/src.txt", tmpl), *src_uri = fr_uri_from_path(src);
    char *cache = fr_format("%s/cache", tmpl);
    put(src, "first");
    set_mtime(src, 1700000000);

    /* fresh only with the same size and mtime, and never without an mtime */
    CHECK(!fr_opencache_fresh(src, 5, 0));
    CHECK(fr_opencache_fresh(src, 5, 1700000000));
    CHECK(!fr_opencache_fresh(src, 6, 1700000000));
    CHECK(!fr_opencache_fresh(src, 5, 1700000001));
    CHECK(!fr_opencache_fresh(tmpl, 0, 1700000000));

    fr_jobs_start(NULL);

    /* a copy, read-only, with the source's mtime, in a folder made for it */
    char *copy = fr_opencache_path(cache, src_uri, "src.txt");
    fr_job_status st = fetch(src_uri, copy);
    CHECK(st.state == JOB_DONE && st.failed == 0);
    char *t = get(copy);
    CHECK_STR(t, "first");
    free(t);
    struct stat sb;
    CHECK(stat(copy, &sb) == 0 && (sb.st_mode & 07777) == 0400 && sb.st_mtime == 1700000000);
    ino_t first_inode = sb.st_ino;

    /* unchanged: the copy stays */
    st = fetch(src_uri, copy);
    CHECK(st.state == JOB_DONE && st.failed == 0);
    CHECK(stat(copy, &sb) == 0 && sb.st_ino == first_inode);

    /* changed: taken again, over the read-only copy */
    put(src, "second!");
    set_mtime(src, 1700000100);
    st = fetch(src_uri, copy);
    CHECK(st.state == JOB_DONE && st.failed == 0);
    t = get(copy);
    CHECK_STR(t, "second!");
    free(t);

    /* a file inside an archive, the way the window opens one */
    char *zip = fr_format("%s/a.zip", tmpl), *zip_uri = fr_uri_from_path(zip);
    fr_strs one = {0};
    FR_PUSH(one, fr_strdup(src_uri));
    fr_job_submit_compress(&one, zip_uri);
    fr_jobs_wait_idle();
    fr_job_status all[8];
    for (int i = 0, k = fr_jobs_list(all, 8); i < k; i++) fr_job_dismiss(all[i].id);
    fr_strs_free(&one);
    char *inner = fr_format("archive://%s!/src.txt", zip_uri), *inner_copy = fr_opencache_path(cache, inner, "src.txt");
    CHECK(strcmp(inner_copy, copy) != 0);
    st = fetch(inner, inner_copy);
    CHECK(st.state == JOB_DONE && st.failed == 0);
    t = get(inner_copy);
    CHECK_STR(t, "second!");
    free(t);

    /* a folder is no file to open; something gone fails */
    char *folder = fr_uri_from_path(tmpl);
    st = fetch(folder, copy);
    CHECK(st.failed == 1);
    free(folder);
    char *gone = fr_format("%s/gone.txt", src_uri);
    st = fetch(gone, copy);
    CHECK(st.failed == 1);
    free(gone);

    /* cleaning: only the entries last used before the cutoff go */
    char *old_dir = fr_strndup(copy, (size_t)(strrchr(copy, '/') - copy));
    char *new_dir = fr_strndup(inner_copy, (size_t)(strrchr(inner_copy, '/') - inner_copy));
    time_t now = time(NULL);
    set_mtime(old_dir, now - FR_OPENCACHE_MAX_AGE - 60);
    CHECK(fr_opencache_clean(cache, now - FR_OPENCACHE_MAX_AGE) == 1);
    CHECK(access(old_dir, F_OK) != 0);
    CHECK(access(inner_copy, F_OK) == 0);
    /* a use keeps it */
    set_mtime(new_dir, now - FR_OPENCACHE_MAX_AGE - 60);
    fr_opencache_touch(inner_copy);
    CHECK(fr_opencache_clean(cache, now - FR_OPENCACHE_MAX_AGE) == 0);
    CHECK(fr_opencache_clean(cache, now + 60) == 1);
    CHECK(access(new_dir, F_OK) != 0);
    CHECK(fr_opencache_clean("/nonexistent/ferret", now) == 0);

    fr_jobs_stop();
    unlink(src), unlink(zip), rmdir(cache), rmdir(tmpl);
    free(src), free(src_uri), free(cache), free(copy), free(zip), free(zip_uri), free(inner), free(inner_copy);
    free(old_dir), free(new_dir);
    return CHECK_DONE();
}

#define _GNU_SOURCE
#include "check.h"
#include "jobs/jobs.h"
#include "vfs/trash.h"
#include "vfs/vfs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void write_path(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static int exists(const char *path)
{
    struct stat st;
    return !lstat(path, &st);
}

int main(void)
{
    /* ---- .trashinfo ---- */
    char *path;
    int64_t when;
    CHECK(fr_trashinfo_parse("[Trash Info]\nPath=/home/u/a%20b%25.txt\nDeletionDate=2024-03-05T14:07:09\n", &path, &when) == 0);
    CHECK_STR(path, "/home/u/a b%.txt");
    struct tm tm = {.tm_year = 124, .tm_mon = 2, .tm_mday = 5, .tm_hour = 14, .tm_min = 7, .tm_sec = 9, .tm_isdst = -1};
    CHECK(when == (int64_t)mktime(&tm));
    free(path);
    CHECK(fr_trashinfo_parse("[Trash Info]\r\nPath=photos/%E2%82%AC.jpg\r\n", &path, &when) == 0);
    CHECK_STR(path, "photos/€.jpg");
    CHECK(when == -1);
    free(path);
    CHECK(fr_trashinfo_parse("[Other]\nPath=/x\n", &path, &when) == -1 && !path);
    CHECK(fr_trashinfo_parse("[Trash Info]\nDeletionDate=2024-03-05T14:07:09\n", &path, &when) == -1);
    CHECK(fr_trashinfo_parse("[Trash Info]\nPath=/bad%zz%4\n", &path, &when) == 0);
    CHECK_STR(path, "/bad%zz%4");
    free(path);
    CHECK(fr_trash_is_root("trash:///") && fr_trash_is_root("trash://") && !fr_trash_is_root("trash:///a"));

    /* ---- trash:/// over a home trash in a scratch folder ---- */
    char dir[] = "/tmp/ferret-trash-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char *data = fr_format("%s/data", dir), *docs = fr_format("%s/docs", dir);
    setenv("XDG_DATA_HOME", data, 1);
    fr_mkdirs(docs);
    char *a = fr_format("%s/a.txt", docs), *sub = fr_format("%s/sub", docs), *inner = fr_format("%s/sub/in.txt", docs);
    write_path(a, "first");
    fr_mkdirs(sub);
    write_path(inner, "inner");
    char *a_uri = fr_uri_from_path(a), *sub_uri = fr_uri_from_path(sub);
    CHECK(fr_vfs_trash(a_uri) == 0);
    write_path(a, "second");
    CHECK(fr_vfs_trash(a_uri) == 0);
    CHECK(fr_vfs_trash(sub_uri) == 0);
    CHECK(!exists(a) && !exists(sub));

    fr_entries es = {0};
    CHECK(fr_vfs_list(FR_TRASH_URI, &es) == 0);
    CHECK(es.n == 3);
    int saw_a = 0, saw_a2 = 0, saw_sub = 0;
    time_t now = time(NULL);
    for (int i = 0; i < es.n; i++) {
        saw_a |= !strcmp(es.v[i].name, "a.txt");
        saw_a2 |= !strcmp(es.v[i].name, "a.2.txt");
        saw_sub |= !strcmp(es.v[i].name, "sub") && es.v[i].is_dir;
        CHECK(es.v[i].mtime > now - 60 && es.v[i].mtime <= now + 1);
    }
    CHECK(saw_a && saw_a2 && saw_sub);
    fr_entries_free(&es);

    fr_entry e = {0};
    CHECK(fr_vfs_stat(FR_TRASH_URI, &e) == 0 && e.is_dir);
    fr_entry_free(&e);
    CHECK(fr_vfs_stat("trash:///a.2.txt", &e) == 0 && !e.is_dir && e.size == 6);
    CHECK_STR(e.name, "a.2.txt");
    fr_entry_free(&e);
    CHECK(fr_vfs_list("trash:///sub", &es) == 0 && es.n == 1);
    fr_entries_free(&es);
    int err = 0;
    fr_stream *s = fr_vfs_open_read("trash:///sub/in.txt", &err);
    CHECK(s != NULL);
    if (s) {
        char buf[16] = {0};
        CHECK(s->read(s, buf, sizeof buf) == 5 && !strcmp(buf, "inner"));
        s->close(s);
    }
    CHECK(fr_vfs_open_write("trash:///new", 1, &err) == NULL && err == -ENOTSUP);
    CHECK(fr_vfs_stat("trash:///nothing", &e) == -ENOENT);
    CHECK(fr_vfs_stat("trash:///..", &e) == -ENOENT);

    CHECK(fr_trash_origin("trash:///a.2.txt", &path, &when) == 0);
    CHECK_STR(path, a);
    CHECK(when > now - 60);
    free(path);

    /* restoring puts it back; the second one finds the name taken and keeps both */
    char *to = NULL;
    CHECK(fr_trash_restore("trash:///a.txt", &to) == 0);
    CHECK_STR(to, a_uri);
    free(to);
    CHECK(fr_trash_restore("trash:///a.2.txt", &to) == 0);
    char *kept = fr_format("file://%s/a (2).txt", docs);
    CHECK_STR(to, kept);
    free(to), free(kept);
    char *info = fr_format("%s/Trash/info/a.txt.trashinfo", data);
    CHECK(!exists(info));
    free(info);
    CHECK(fr_trash_restore("trash:///a.txt", &to) == -ENOENT);

    /* a folder whose parent went comes back with it */
    char *docs_uri = fr_uri_from_path(docs), *moved = fr_format("%s/moved", dir);
    CHECK(rename(docs, moved) == 0);
    free(moved);
    CHECK(fr_trash_restore("trash:///sub", &to) == 0);
    CHECK(exists(inner));
    free(to);

    /* deleting for good takes the item and its info */
    write_path(a, "third");
    CHECK(fr_vfs_trash(a_uri) == 0);
    CHECK(fr_vfs_remove("trash:///a.txt") == 0);
    CHECK(fr_vfs_list(FR_TRASH_URI, &es) == 0 && es.n == 0);
    fr_entries_free(&es);
    info = fr_format("%s/Trash/info/a.txt.trashinfo", data);
    CHECK(!exists(info));
    free(info);

    /* a folder deleted for good through the jobs, as emptying the trash does */
    char *moved_sub = fr_format("file://%s/sub", docs);
    CHECK(fr_vfs_trash(moved_sub) == 0);
    fr_jobs_start(NULL);
    fr_strs items = {0};
    FR_PUSH(items, fr_strdup("trash:///sub"));
    fr_job_submit(JOB_DELETE, &items, NULL);
    fr_jobs_wait_idle();
    fr_job_status st;
    CHECK(fr_jobs_list(&st, 1) == 1 && st.failed == 0);
    CHECK(fr_vfs_list(FR_TRASH_URI, &es) == 0 && es.n == 0);
    fr_entries_free(&es);
    fr_strs_free(&items);
    free(moved_sub);

    char *root = fr_uri_from_path(dir);
    FR_PUSH(items, root);
    fr_job_submit(JOB_DELETE, &items, NULL);
    fr_jobs_wait_idle();
    CHECK(!exists(dir));
    fr_jobs_stop();
    fr_strs_free(&items);
    free(data), free(docs), free(a), free(sub), free(inner), free(a_uri), free(sub_uri), free(docs_uri);
    return CHECK_DONE();
}

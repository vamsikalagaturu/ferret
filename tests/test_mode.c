#define _GNU_SOURCE
#include "check.h"
#include "jobs/jobs.h"
#include "vfs/local_info.h"
#include "vfs/vfs.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[64];

static char *at(const char *rel) { return fr_format("%s/%s", root, rel); }

static void put(const char *rel, const char *text)
{
    char *path = at(rel), *uri = fr_uri_from_path(path);
    int err = 0;
    fr_stream *s = fr_vfs_open_write(uri, 0, &err);
    CHECK(s != NULL);
    if (s) s->write(s, text, strlen(text)), s->close(s);
    free(path), free(uri);
}

static unsigned mode_of(const char *rel)
{
    char *path = at(rel);
    struct stat st;
    unsigned m = lstat(path, &st) ? 0xffffffffu : (unsigned)(st.st_mode & 07777);
    free(path);
    return m;
}

static void set(const char *rel, unsigned mode)
{
    char *path = at(rel);
    CHECK(chmod(path, mode) == 0);
    free(path);
}

static void strings(void)
{
    char rwx[10], oct[6];
    fr_mode_rwx(0755, rwx);
    CHECK_STR(rwx, "rwxr-xr-x");
    fr_mode_rwx(04755, rwx);
    CHECK_STR(rwx, "rwsr-xr-x");
    fr_mode_rwx(02640, rwx);
    CHECK_STR(rwx, "rw-r-S---");
    fr_mode_rwx(01777, rwx);
    CHECK_STR(rwx, "rwxrwxrwt");
    fr_mode_rwx(01776, rwx);
    CHECK_STR(rwx, "rwxrwxrwT");
    fr_mode_rwx(0, rwx);
    CHECK_STR(rwx, "---------");
    fr_mode_octal(0755, oct);
    CHECK_STR(oct, "0755");
    fr_mode_octal(04711, oct);
    CHECK_STR(oct, "4711");
    fr_mode_octal(0100644, oct);
    CHECK_STR(oct, "0644");

    CHECK(fr_mode_edit(0644, 0100, 0, 0) == 0744);
    CHECK(fr_mode_edit(0777, 0, 0077, 0) == 0700);
    CHECK(fr_mode_edit(04755, 0, 0022, 0) == 04755);
    CHECK(fr_mode_edit(0700, 0640, 0137, 0) == 0640);
    CHECK(fr_mode_edit(0700, 0640, 0137, 1) == 0750);
    CHECK(fr_mode_edit(0000, 0444, 0333, 1) == 0555);
    CHECK(fr_mode_edit(0000, 0200, 0577, 1) == 0200);
}

static void mounts(void)
{
    const char *table = "/dev/sda2 / ext4 rw,relatime 0 0\n"
                        "proc /proc proc rw 0 0\n"
                        "/dev/sdb1 /media/my\\040disk vfat rw 0 0\n"
                        "/dev/sda3 /home btrfs rw 0 0\n"
                        "/dev/sda4 /home btrfs rw 0 0\n";
    fr_mount m;
    CHECK(fr_mount_find(table, "/home/a/b", &m) == 0);
    CHECK_STR(m.point, "/home");
    CHECK_STR(m.source, "/dev/sda4");
    CHECK_STR(m.type, "btrfs");
    CHECK(fr_mount_find(table, "/homework", &m) == 0);
    CHECK_STR(m.point, "/");
    CHECK(fr_mount_find(table, "/media/my disk/x.jpg", &m) == 0);
    CHECK_STR(m.point, "/media/my disk");
    CHECK_STR(m.type, "vfat");
    CHECK(fr_mount_find(table, "/proc", &m) == 0);
    CHECK_STR(m.type, "proc");
    CHECK(fr_mount_find("", "/x", &m) == -ENOENT);
}

static void tree(void)
{
    char *dir = at("d"), *sub = at("d/sub"), *uri = fr_uri_from_path(dir);
    CHECK(mkdir(dir, 0755) == 0);
    CHECK(mkdir(sub, 0755) == 0);
    put("d/a.txt", "hello");
    put("d/sub/b.txt", "hi");
    put("d/sub/c.txt", "");
    char *link = at("d/link"), *nowhere = at("nowhere");
    CHECK(symlink("a.txt", link) == 0);

    fr_tree_count c = {0};
    int cancel = 0;
    fr_vfs_measure(uri, &c, &cancel);
    CHECK(c.folders == 2);
    CHECK(c.files == 4);
    CHECK(c.bytes == 7);
    CHECK(c.unreadable == 0);
    char *gone = fr_uri_from_path(nowhere);
    fr_tree_count g = {0};
    fr_vfs_measure(gone, &g, &cancel);
    CHECK(g.unreadable == 1 && g.files == 0);

    fr_local_info info;
    CHECK(fr_local_info_get(link, &info) == 0);
    CHECK(info.is_link && info.target_ok);
    CHECK_STR(info.target, "a.txt");
    CHECK(info.target_mode == mode_of("d/a.txt"));
    CHECK(fr_local_info_get(dir, &info) == 0);
    CHECK(info.is_dir && info.mode == 0755 && info.uid == getuid() && info.links >= 1 && info.has_mount);
    CHECK(fr_local_info_get(nowhere, &info) == -ENOENT);

    char *a = at("d/a.txt"), *au = fr_uri_from_path(a);
    CHECK(fr_vfs_set_mode(au, 0600) == 0);
    CHECK(mode_of("d/a.txt") == 0600);
    CHECK(fr_vfs_set_mode("nope://x", 0600) == -EPROTONOSUPPORT);

    /* the tree: a folder that cannot be searched below must still be reached and opened */
    set("d/sub/b.txt", 0600);
    set("d/sub", 0600);
    fr_jobs_start(NULL);
    fr_strs srcs = {0};
    FR_PUSH(srcs, fr_strdup(uri));
    fr_job_submit_chmod(&srcs, 0640, 0137);
    fr_jobs_wait_idle();
    fr_job_status st[4];
    CHECK(fr_jobs_list(st, 4) == 1 && st[0].kind == JOB_CHMOD && st[0].state == JOB_DONE && st[0].failed == 0);
    fr_job_dismiss(st[0].id);
    CHECK(mode_of("d") == 0640);
    set("d", 0755);
    CHECK(mode_of("d/sub") == 0750);
    CHECK(mode_of("d/sub/b.txt") == 0640);
    CHECK(mode_of("d/sub/c.txt") == 0640);
    CHECK(mode_of("d/a.txt") == 0640);
    CHECK((mode_of("d/link") & 0777) == 0777);

    fr_job_submit_chmod(&srcs, 0755, 0022);
    fr_jobs_wait_idle();
    CHECK(fr_jobs_list(st, 4) == 1 && st[0].failed == 0);
    fr_job_dismiss(st[0].id);
    CHECK(mode_of("d/sub") == 0755 && mode_of("d/sub/b.txt") == 0755);
    fr_jobs_stop();
    fr_strs_free(&srcs);

    char *b = at("d/sub/b.txt"), *cc = at("d/sub/c.txt");
    CHECK(unlink(link) == 0 && unlink(a) == 0 && unlink(b) == 0 && unlink(cc) == 0);
    CHECK(rmdir(sub) == 0 && rmdir(dir) == 0);
    free(dir), free(sub), free(uri), free(link), free(nowhere), free(gone), free(a), free(au), free(b), free(cc);
}

int main(void)
{
    strings();
    mounts();
    snprintf(root, sizeof root, "/tmp/ferret-mode-XXXXXX");
    CHECK(mkdtemp(root) != NULL);
    tree();
    CHECK(rmdir(root) == 0);
    return CHECK_DONE();
}

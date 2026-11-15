#define _GNU_SOURCE
#include "check.h"
#include "jobs/compare.h"
#include "jobs/gitstatus.h"
#include "jobs/jobs.h"
#include "jobs/perms.h"
#include "vfs/vfs.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

static char *dir; /* the test's folder, a path */

static char *path(const char *rel) { return fr_format("%s/%s", dir, rel); }
static char *uri(const char *rel)
{
    char *p = path(rel), *u = fr_uri_from_path(p);
    free(p);
    return u;
}

static void put_at(const char *rel, const char *text, int64_t mtime)
{
    char *p = path(rel);
    FILE *f = fopen(p, "w");
    CHECK(f != NULL);
    if (f) fputs(text, f), fclose(f);
    if (mtime) {
        struct utimbuf t = {(time_t)mtime, (time_t)mtime};
        utime(p, &t);
    }
    free(p);
}

static void mk(const char *rel)
{
    char *p = path(rel);
    CHECK(mkdir(p, 0755) == 0);
    free(p);
}

static char *get(const char *rel)
{
    char *p = path(rel);
    FILE *f = fopen(p, "r");
    free(p);
    if (!f) return NULL;
    char buf[256];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return fr_strndup(buf, n);
}

static int there(const char *rel)
{
    char *p = path(rel);
    struct stat st;
    int r = !lstat(p, &st);
    free(p);
    return r;
}

#define CHECK_FILE(rel, text)                                                                                          \
    do {                                                                                                               \
        char *got_ = get(rel);                                                                                         \
        CHECK_STR(got_, text);                                                                                         \
        free(got_);                                                                                                    \
    } while (0)

static fr_job_status wait_for(int id)
{
    fr_jobs_wait_idle();
    fr_job_status st[8], r = {0};
    int k = fr_jobs_list(st, 8);
    for (int i = 0; i < k; i++)
        if (st[i].id == id) r = st[i];
    return r;
}

/* runs the job, then its log's inverse as undo does; the log of each into *log and *undone when given */
static fr_job_status finish(int id, fr_steps *log)
{
    fr_job_status st = wait_for(id);
    fr_steps l = {0};
    CHECK(fr_job_take_log(id, &l) == 0);
    fr_job_dismiss(id);
    if (log) *log = l;
    else fr_steps_free(&l);
    return st;
}

static fr_job_status undo(const fr_steps *log, fr_steps *redo_log)
{
    fr_steps inv = fr_steps_inverse(log);
    int id = fr_job_submit_steps(&inv, "Undoing");
    fr_steps_free(&inv);
    return finish(id, redo_log);
}

static void classify(void)
{
    fr_entry a = {.name = "a", .size = 10, .mtime = 1000}, b = {.name = "a", .size = 10, .mtime = 1001};
    CHECK(fr_cmp_classify(&a, NULL, 2, -1) == CMP_ONLY);
    CHECK(fr_cmp_classify(NULL, &a, 2, -1) == CMP_NONE);
    CHECK(fr_cmp_classify(&a, &b, 2, -1) == CMP_SAME);
    CHECK(fr_cmp_classify(&a, &b, 0, -1) == CMP_OLDER);
    CHECK(fr_cmp_classify(&b, &a, 0, -1) == CMP_NEWER);
    b.mtime = 5000;
    CHECK(fr_cmp_classify(&a, &b, 2, -1) == CMP_OLDER);
    CHECK(fr_cmp_classify(&a, &b, 2, 1) == CMP_SAME); /* the bytes say alike, whatever the times */
    CHECK(fr_cmp_classify(&b, &a, 2, 0) == CMP_NEWER);
    b.mtime = 1000, b.size = 11;
    CHECK(fr_cmp_classify(&a, &b, 2, -1) == CMP_DIFFERENT);
    a.size = 11;
    CHECK(fr_cmp_classify(&a, &b, 2, 0) == CMP_DIFFERENT);
    fr_entry d = {.name = "a", .is_dir = 1}, d2 = {.name = "a", .is_dir = 1}, l = {.name = "a", .is_link = 1, .size = 11, .mtime = 1000};
    CHECK(fr_cmp_classify(&d, &d2, 2, -1) == CMP_FOLDER);
    CHECK(fr_cmp_classify(&d, &a, 2, -1) == CMP_DIFFERENT);
    CHECK(fr_cmp_classify(&l, &a, 2, -1) == CMP_DIFFERENT);
}

static const fr_cmp_item *find_path(const fr_cmp_items *it, const char *p)
{
    for (int i = 0; i < it->n; i++)
        if (!strcmp(it->v[i].path, p)) return &it->v[i];
    return NULL;
}

/* L and R:
 *   only_l (L), only_r (R), same (both, alike), newer (L newer), older (L older), differ (same time, other size),
 *   samesize (same size and time, other bytes), sub/ (both: sub/x alike, sub/y only in L, sub/deep/z newer in R),
 *   flat/ (both, alike inside), lonely/ (L only, with a file) */
static void make_trees(void)
{
    mk("L"), mk("R");
    put_at("L/only_l", "l", 1000);
    put_at("R/only_r", "r", 1000);
    put_at("L/same", "same", 1000), put_at("R/same", "same", 1001);
    put_at("L/newer", "new!", 9000), put_at("R/newer", "old", 1000);
    put_at("L/older", "old", 1000), put_at("R/older", "new!", 9000);
    put_at("L/differ", "abc", 1000), put_at("R/differ", "abcd", 1000);
    put_at("L/samesize", "aaaa", 1000), put_at("R/samesize", "bbbb", 1000);
    mk("L/sub"), mk("R/sub"), mk("L/sub/deep"), mk("R/sub/deep");
    put_at("L/sub/x", "x", 1000), put_at("R/sub/x", "x", 1000);
    put_at("L/sub/y", "y", 1000);
    put_at("L/sub/deep/z", "z", 1000), put_at("R/sub/deep/z", "zz", 9000);
    mk("L/flat"), mk("R/flat");
    put_at("L/flat/f", "f", 1000), put_at("R/flat/f", "f", 1000);
    mk("L/lonely");
    put_at("L/lonely/k", "k", 1000);
}

static void compare_trees(void)
{
    char *l = uri("L"), *r = uri("R");
    fr_cmp_options o = {.recursive = 0, .tolerance = FR_CMP_TOLERANCE};
    fr_cmp_items it = {0};
    int64_t scanned = 0;
    int cancel = 0;
    CHECK(fr_cmp_folders(l, r, &o, &it, &scanned, &cancel) == 0);
    CHECK(it.top == it.n && it.n == 10);
    CHECK(scanned == 10);
    const fr_cmp_item *x;
    CHECK((x = fr_cmp_find(&it, "only_l")) && x->left == CMP_ONLY && x->right == CMP_NONE);
    CHECK((x = fr_cmp_find(&it, "only_r")) && x->left == CMP_NONE && x->right == CMP_ONLY);
    CHECK((x = fr_cmp_find(&it, "same")) && x->left == CMP_SAME && x->right == CMP_SAME);
    CHECK((x = fr_cmp_find(&it, "newer")) && x->left == CMP_NEWER && x->right == CMP_OLDER);
    CHECK((x = fr_cmp_find(&it, "older")) && x->left == CMP_OLDER && x->right == CMP_NEWER);
    CHECK((x = fr_cmp_find(&it, "differ")) && x->left == CMP_DIFFERENT);
    CHECK((x = fr_cmp_find(&it, "samesize")) && x->left == CMP_SAME); /* without reading them */
    CHECK((x = fr_cmp_find(&it, "sub")) && x->left == CMP_FOLDER);
    CHECK(fr_cmp_find(&it, "nothing") == NULL);
    fr_cmp_items_free(&it);

    o.recursive = 1, o.content = 1, scanned = 0;
    CHECK(fr_cmp_folders(l, r, &o, &it, &scanned, &cancel) == 0);
    CHECK(it.top == 10);
    CHECK((x = fr_cmp_find(&it, "samesize")) && x->left == CMP_DIFFERENT);
    CHECK((x = fr_cmp_find(&it, "same")) && x->left == CMP_SAME);
    CHECK((x = fr_cmp_find(&it, "sub")) && x->left == CMP_INSIDE && x->right == CMP_INSIDE);
    CHECK((x = fr_cmp_find(&it, "flat")) && x->left == CMP_SAME);
    CHECK((x = fr_cmp_find(&it, "lonely")) && x->left == CMP_ONLY);
    /* below the top only what differs, and nothing inside a folder one side lacks */
    CHECK(find_path(&it, "sub/x") == NULL);
    CHECK(find_path(&it, "flat/f") == NULL);
    CHECK(find_path(&it, "lonely/k") == NULL);
    CHECK((x = find_path(&it, "sub/y")) && x->left == CMP_ONLY);
    CHECK((x = find_path(&it, "sub/deep")) && x->left == CMP_INSIDE);
    CHECK((x = find_path(&it, "sub/deep/z")) && x->left == CMP_OLDER && x->right == CMP_NEWER);
    CHECK(scanned > 10);

    fr_sync_counts c;
    fr_steps plan = fr_sync_plan(&it, l, r, 1, 0, &c);
    /* L -> R: only_l, lonely, sub/y copied; newer replaced; older, differ, samesize, sub/deep/z skipped */
    CHECK(c.copy == 3 && c.replace == 1 && c.remove == 0 && c.skipped == 4 && c.extra == 1);
    for (int i = 0; i < plan.n; i++) CHECK(plan.v[i].kind == STEP_COPIED || plan.v[i].kind == STEP_REPLACED);
    fr_steps_free(&plan);
    plan = fr_sync_plan(&it, l, r, 1, 1, &c);
    CHECK(c.copy == 3 && c.replace == 5 && c.remove == 1 && c.extra == 1);
    fr_steps_free(&plan);
    plan = fr_sync_plan(&it, l, r, 0, 0, &c);
    /* R -> L: only_r copied; older (R newer) and sub/deep/z replaced */
    CHECK(c.copy == 1 && c.replace == 2 && c.extra == 3);
    fr_steps_free(&plan);

    /* the sync L -> R, and taking it back */
    plan = fr_sync_plan(&it, l, r, 1, 0, &c);
    fr_steps log = {0}, redo = {0};
    fr_job_status st = finish(fr_job_submit_sync(&plan, r), &log);
    CHECK(st.state == JOB_DONE && st.failed == 0);
    CHECK_FILE("R/only_l", "l");
    CHECK_FILE("R/newer", "new!");
    CHECK_FILE("R/lonely/k", "k");
    CHECK_FILE("R/sub/y", "y");
    CHECK_FILE("R/older", "new!");
    CHECK_FILE("R/differ", "abcd");
    CHECK_FILE("R/sub/deep/z", "zz");
    CHECK(log.n == 4);
    st = undo(&log, &redo);
    CHECK(st.state == JOB_DONE && st.failed == 0);
    CHECK(!there("R/only_l") && !there("R/lonely") && !there("R/sub/y"));
    CHECK_FILE("R/newer", "old");
    CHECK_FILE("L/newer", "new!");
    CHECK(!there("R/.newer.ferret-undone"));
    /* and redone */
    fr_steps again = {0};
    st = undo(&redo, &again);
    CHECK(st.state == JOB_DONE && st.failed == 0);
    CHECK_FILE("R/newer", "new!");
    CHECK_FILE("R/only_l", "l");
    fr_steps_free(&log), fr_steps_free(&redo), fr_steps_free(&again);
    fr_steps_free(&plan);

    /* a mirror R -> L removes what only L has, to the trash */
    fr_cmp_items_free(&it);
    CHECK(fr_cmp_folders(l, r, &o, &it, &scanned, &cancel) == 0);
    plan = fr_sync_plan(&it, l, r, 0, 1, &c);
    CHECK(c.remove == 0); /* L has nothing R lacks now */
    fr_steps_free(&plan);
    put_at("L/extra", "e", 1000);
    fr_cmp_items_free(&it);
    CHECK(fr_cmp_folders(l, r, &o, &it, &scanned, &cancel) == 0);
    plan = fr_sync_plan(&it, l, r, 0, 1, &c);
    CHECK(c.remove == 1);
    st = finish(fr_job_submit_sync(&plan, l), &log);
    CHECK(st.state == JOB_DONE && st.failed == 0);
    CHECK(!there("L/extra"));
    CHECK_FILE("L/differ", "abcd");
    CHECK_FILE("L/older", "new!");
    st = undo(&log, NULL);
    CHECK(st.failed == 0);
    CHECK_FILE("L/extra", "e");
    CHECK_FILE("L/differ", "abc");
    fr_steps_free(&log);
    fr_steps_free(&plan);
    fr_cmp_items_free(&it);

    /* a copy whose name was taken since asks; skipped here */
    put_at("L/late", "L", 1000);
    fr_steps one = {0};
    char *a = uri("L/late"), *b = uri("R/late");
    fr_steps_add(&one, STEP_COPIED, a, b);
    put_at("R/late", "R", 1000);
    int id = fr_job_submit_sync(&one, r);
    for (int tries = 0; tries < 2000; tries++) {
        fr_job_status s[8];
        int k = fr_jobs_list(s, 8), asking = 0;
        for (int i = 0; i < k; i++)
            if (s[i].id == id && s[i].state == JOB_ASKING) asking = 1;
        if (asking) {
            fr_job_answer(id, CONFLICT_SKIP, 0);
            break;
        }
        usleep(1000);
    }
    st = finish(id, NULL);
    CHECK(st.state == JOB_DONE);
    CHECK_FILE("R/late", "R");
    free(a), free(b);
    fr_steps_free(&one);
    free(l), free(r);
}

static void relative(const char *from, const char *to, const char *want)
{
    char *s = fr_path_relative(from, to);
    CHECK_STR(s, want);
    free(s);
}

static void links(void)
{
    relative("/a/b", "/a/c/d", "../c/d");
    relative("/a/b", "/a/b/c", "c");
    relative("/a/b", "/a/b", ".");
    relative("/a/b/c", "/a", "../..");
    relative("/", "/x/y", "x/y");
    relative("/x/y", "/", "../..");
    relative("/ab/c", "/a/c", "../../a/c");
    relative("/a//b/", "/a/b/c/", "c");

    mk("ln"), mk("ln/src"), mk("ln/out");
    put_at("ln/src/f.txt", "F", 0);
    put_at("ln/out/f.txt", "taken", 0);
    char *src = uri("ln/src/f.txt"), *srcdir = uri("ln/src"), *out = uri("ln/out");
    fr_strs srcs = {0};
    FR_PUSH(srcs, fr_strdup(src));
    FR_PUSH(srcs, fr_strdup(srcdir));
    int id = fr_job_submit_link(&srcs, out, 1);
    /* f.txt is taken: replaced, the old one to the trash */
    for (int tries = 0; tries < 2000; tries++) {
        fr_job_status st[8];
        int k = fr_jobs_list(st, 8), asking = 0;
        for (int i = 0; i < k; i++)
            if (st[i].id == id && st[i].state == JOB_ASKING) asking = 1;
        if (asking) {
            fr_job_answer(id, CONFLICT_REPLACE, 0);
            break;
        }
        usleep(1000);
    }
    fr_steps log = {0};
    fr_job_status st = finish(id, &log);
    CHECK(st.state == JOB_DONE && st.failed == 0);
    char buf[256], *lp = path("ln/out/f.txt");
    ssize_t n = readlink(lp, buf, sizeof buf - 1);
    CHECK(n > 0);
    buf[n > 0 ? n : 0] = 0;
    CHECK_STR(buf, "../src/f.txt");
    CHECK_FILE("ln/out/f.txt", "F");
    char *dp = path("ln/out/src");
    n = readlink(dp, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    CHECK_STR(buf, "../src");
    CHECK(log.n == 3 && log.v[0].kind == STEP_TRASH && log.v[1].kind == STEP_LINKED);
    st = undo(&log, NULL);
    CHECK(st.failed == 0);
    CHECK_FILE("ln/out/f.txt", "taken");
    CHECK(!there("ln/out/src"));
    fr_steps_free(&log);
    fr_strs_free(&srcs);

    /* absolute, in the same folder: a name of its own */
    FR_PUSH(srcs, fr_strdup(src));
    st = finish(fr_job_submit_link(&srcs, srcdir, 0), NULL);
    CHECK(st.failed == 0);
    char *lp2 = path("ln/src/f (2).txt"), *abs = path("ln/src/f.txt");
    n = readlink(lp2, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    CHECK_STR(buf, abs);
    fr_strs_free(&srcs);
    free(src), free(srcdir), free(out), free(lp), free(dp), free(lp2), free(abs);
}

static void perms(void)
{
    fr_perm_change c = {.set = 0750, .clear = 0027, .uid = -1, .gid = -1};
    CHECK(fr_perm_mode(&c, 0644, 0, 1) == 0750);
    CHECK(fr_perm_mode(&c, 04644, 0, 1) == 04750);
    c = (fr_perm_change){.set = 0444, .clear = 0222, .uid = -1, .gid = -1};
    CHECK(fr_perm_mode(&c, 0700, 1, 0) == 0555); /* a folder inside gets search where it gets read */
    CHECK(fr_perm_mode(&c, 0700, 1, 1) == 0544);
    c = (fr_perm_change){.set = 0111, .exec_x = 1, .uid = -1, .gid = -1};
    CHECK(fr_perm_mode(&c, 0644, 0, 1) == 0644); /* X: a file nobody executes stays so */
    CHECK(fr_perm_mode(&c, 0744, 0, 1) == 0755);
    CHECK(fr_perm_mode(&c, 0600, 1, 1) == 0711);
    c.only = PERM_FILES, c.exec_x = 0;
    CHECK(fr_perm_mode(&c, 0600, 1, 1) == 0600);
    CHECK(fr_perm_mode(&c, 0600, 0, 1) == 0711);
    c.only = PERM_FOLDERS;
    CHECK(fr_perm_mode(&c, 0600, 0, 1) == 0600);
    CHECK(fr_perm_mode(&c, 0600, 1, 0) == 0711);
    /* mixed bits that are neither set nor cleared stay each item's own */
    c = (fr_perm_change){.set = 0004, .clear = 0002, .uid = -1, .gid = -1};
    CHECK(fr_perm_mode(&c, 0760, 0, 1) == 0764);
    CHECK(fr_perm_mode(&c, 0607, 0, 1) == 0605);

    uint32_t m;
    int special;
    CHECK(fr_mode_parse_octal("755", &m, &special) && m == 0755 && !special);
    CHECK(fr_mode_parse_octal("4755", &m, &special) && m == 04755 && special);
    CHECK(fr_mode_parse_octal("0644", &m, &special) && m == 0644 && special);
    CHECK(!fr_mode_parse_octal("75", &m, &special));
    CHECK(!fr_mode_parse_octal("758", &m, &special));
    CHECK(!fr_mode_parse_octal("07555", &m, &special));
    uint32_t id;
    CHECK(fr_owner_lookup("root", 0, &id) == 0 && id == 0);
    CHECK(fr_owner_lookup("1234", 1, &id) == 0 && id == 1234);
    CHECK(fr_owner_lookup("no-such-user-here", 0, &id) == -ENOENT);
    char name[64];
    fr_owner_name(0, 0, name, sizeof name);
    CHECK_STR(name, "root");

    /* not down the tree, files only, X, then undone */
    mk("p"), mk("p/d");
    put_at("p/a", "a", 0), put_at("p/d/b", "b", 0), put_at("p/x", "x", 0);
    char *pa = path("p/a"), *pd = path("p/d"), *pb = path("p/d/b"), *px = path("p/x");
    chmod(pa, 0644), chmod(pd, 0755), chmod(pb, 0600), chmod(px, 0700);
    fr_strs srcs = {0};
    FR_PUSH(srcs, uri("p/a"));
    FR_PUSH(srcs, uri("p/d"));
    FR_PUSH(srcs, uri("p/x"));
    c = (fr_perm_change){.set = 0011, .clear = 0002, .uid = -1, .gid = -1, .exec_x = 1};
    fr_steps log = {0};
    fr_job_status st = finish(fr_job_submit_perms(&srcs, &c), &log);
    CHECK(st.failed == 0);
    struct stat s;
    CHECK(!stat(pa, &s) && (s.st_mode & 07777) == 0644);
    CHECK(!stat(pd, &s) && (s.st_mode & 07777) == 0755);
    CHECK(!stat(pb, &s) && (s.st_mode & 07777) == 0600); /* not recursive */
    CHECK(!stat(px, &s) && (s.st_mode & 07777) == 0711);
    fr_steps_free(&log);
    c = (fr_perm_change){.set = 0040, .recursive = 1, .only = PERM_FILES, .uid = -1, .gid = getgid()};
    st = finish(fr_job_submit_perms(&srcs, &c), &log);
    CHECK(st.failed == 0);
    CHECK(!stat(pb, &s) && (s.st_mode & 07777) == 0640);
    CHECK(!stat(pd, &s) && (s.st_mode & 07777) == 0755);
    st = undo(&log, NULL);
    CHECK(st.failed == 0);
    CHECK(!stat(pb, &s) && (s.st_mode & 07777) == 0600);
    CHECK(!stat(px, &s) && (s.st_mode & 07777) == 0711);
    fr_steps_free(&log);
    fr_strs_free(&srcs);
    free(pa), free(pd), free(pb), free(px);
}

static void git(void)
{
    /* git status --porcelain=v2 -z --ignored=matching, run at the top of a work tree */
    static const char out[] = "1 .M N... 100644 100644 100644 aaa aaa src/a.c\0"
                              "1 M. N... 100644 100644 100644 aaa bbb src/deep/b.c\0"
                              "1 MM N... 100644 100644 100644 aaa bbb top.txt\0"
                              "2 R. N... 100644 100644 100644 aaa aaa R100 src/new name.c\0src/old name.c\0"
                              "u UU N... 100644 100644 100644 100644 a b c src/conflict.c\0"
                              "? src/untracked.c\0"
                              "? src/newdir/\0"
                              "! src/build/\0"
                              "! src/deep/ignored.o\0"
                              "! node_modules/\0"
                              "1 .D N... 100644 100644 000000 aaa aaa src/gone.c\0";
    fr_git_marks m = {0};
    CHECK(fr_git_parse(out, sizeof out - 1, "", &m) == 0);
    CHECK(fr_git_flags(&m, "top.txt") == (GIT_MODIFIED | GIT_STAGED));
    CHECK(fr_git_flags(&m, "src") == (GIT_MODIFIED | GIT_STAGED | GIT_CONFLICT | GIT_UNTRACKED)); /* not ignored */
    CHECK(fr_git_flags(&m, "node_modules") == GIT_IGNORED);
    CHECK(fr_git_flags(&m, "README") == 0);
    CHECK(fr_git_parse(out, sizeof out - 1, "src", &m) == 0);
    CHECK(fr_git_flags(&m, "a.c") == GIT_MODIFIED);
    CHECK(fr_git_flags(&m, "deep") == GIT_STAGED);
    CHECK(fr_git_flags(&m, "new name.c") == GIT_STAGED);
    CHECK(fr_git_flags(&m, "old name.c") == 0);
    CHECK(fr_git_flags(&m, "conflict.c") == GIT_CONFLICT);
    CHECK(fr_git_flags(&m, "untracked.c") == GIT_UNTRACKED);
    CHECK(fr_git_flags(&m, "newdir") == GIT_UNTRACKED);
    CHECK(fr_git_flags(&m, "build") == GIT_IGNORED);
    CHECK(fr_git_flags(&m, "top.txt") == 0);
    CHECK(fr_git_parse(out, sizeof out - 1, "src/deep", &m) == 0);
    CHECK(fr_git_flags(&m, "b.c") == GIT_STAGED);
    CHECK(fr_git_flags(&m, "ignored.o") == GIT_IGNORED);
    /* inside a folder ignored as a whole, everything is */
    CHECK(fr_git_parse(out, sizeof out - 1, "src/build/x", &m) == 0);
    CHECK(fr_git_flags(&m, "anything") == GIT_IGNORED);
    CHECK(fr_git_parse(out, sizeof out - 1, "src/newdir", &m) == 0);
    CHECK(fr_git_flags(&m, "f") == GIT_UNTRACKED);
    CHECK(fr_git_parse(out, sizeof out - 1, "srcx", &m) == 0);
    CHECK(m.n == 0);
    CHECK(fr_git_parse("", 0, "", &m) == 0 && m.n == 0);
    CHECK(fr_git_parse("garbage", 7, "", &m) == -1);
    CHECK(fr_git_parse("1 .M\0", 5, "", &m) == -1);
    fr_git_marks_free(&m);

    /* the top: the nearest folder up with a .git (only looked for, git is not run) */
    char *l = path("L"), *deep = path("L/sub/deep");
    char *top = fr_git_top(deep);
    CHECK(!top || strncmp(top, dir, strlen(dir)));
    free(top);
    mk("L/.git");
    top = fr_git_top(deep);
    CHECK_STR(top, l);
    free(top);
    char *dot = path("L/.git");
    rmdir(dot);
    /* outside any work tree git says so, and nothing is marked */
    top = fr_git_top(dir);
    if (!top) CHECK(fr_git_status(dir, 2000, &m) == -ENOENT && m.n == 0);
    free(top), free(dot), free(l), free(deep);
}

int main(void)
{
    char tmpl[] = "/tmp/ferret-sync-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    dir = tmpl;
    char *data = fr_format("%s/data", tmpl);
    setenv("XDG_DATA_HOME", data, 1);
    fr_jobs_start(NULL);
    classify();
    make_trees();
    compare_trees();
    links();
    perms();
    git();
    fr_jobs_stop();
    free(data);
    return CHECK_DONE();
}

#define _GNU_SOURCE
#include "check.h"
#include "jobs/jobs.h"
#include "vfs/vfs.h"
#include "vfs/vfs_archive.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char *root, *root_path;

static char *at(const char *rel) { return fr_format("%s/%s", root, rel); }

/* archive://file:///<root>/<archive>!/<inner> */
static char *in(const char *archive, const char *inner)
{
    char *outer = at(archive), *r = fr_archive_root(outer), *u = fr_format("%s%s", r, inner);
    free(outer), free(r);
    return u;
}

typedef struct {
    const char *path, *data; /* data NULL: a folder */
} item;

static void make_archive(const char *rel, int (*format)(struct archive *), int (*filter)(struct archive *), const item *items, int n)
{
    char *path = fr_format("%s/%s", root_path, rel);
    locale_t utf8 = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0), previous = utf8 ? uselocale(utf8) : (locale_t)0;
    struct archive *a = archive_write_new();
    format(a);
    if (filter) filter(a);
    CHECK(archive_write_open_filename(a, path) == ARCHIVE_OK);
    for (int i = 0; i < n; i++) {
        struct archive_entry *e = archive_entry_new();
        archive_entry_set_pathname_utf8(e, items[i].path);
        archive_entry_set_filetype(e, items[i].data ? AE_IFREG : AE_IFDIR);
        archive_entry_set_perm(e, items[i].data ? 0644 : 0755);
        archive_entry_set_mtime(e, 1700000000, 0);
        if (items[i].data) archive_entry_set_size(e, (la_int64_t)strlen(items[i].data));
        CHECK(archive_write_header(a, e) == ARCHIVE_OK);
        if (items[i].data) archive_write_data(a, items[i].data, strlen(items[i].data));
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    if (previous) uselocale(previous);
    if (utf8) freelocale(utf8);
    free(path);
}

static char *read_uri(const char *uri)
{
    int err = 0;
    fr_stream *s = fr_vfs_open_read(uri, &err);
    if (!s) return NULL;
    char buf[4096];
    size_t got = 0;
    long n;
    while ((n = s->read(s, buf + got, sizeof buf - 1 - got)) > 0) got += (size_t)n;
    s->close(s);
    return n < 0 ? NULL : fr_strndup(buf, got);
}

static void check_read(const char *uri, const char *want)
{
    char *got = read_uri(uri);
    if (!got || strcmp(got, want)) fprintf(stderr, "reading %s\n", uri);
    CHECK_STR(got, want);
    free(got);
}

static void put(const char *uri, const char *text)
{
    int err = 0;
    fr_stream *s = fr_vfs_open_write(uri, 0, &err);
    CHECK(s != NULL);
    if (s) {
        CHECK(s->write(s, text, strlen(text)) == (long)strlen(text));
        CHECK(s->close(s) == 0);
    }
}

static int has(const fr_entries *es, const char *name, int is_dir)
{
    for (int i = 0; i < es->n; i++)
        if (!strcmp(es->v[i].name, name)) return es->v[i].is_dir == is_dir;
    return 0;
}

static int exists(const char *uri) { return fr_vfs_exists(uri); }

static int exists_in(const char *archive, const char *inner)
{
    char *u = in(archive, inner);
    int r = exists(u);
    free(u);
    return r;
}

static const item sample[] = {
    {"docs/", NULL}, {"docs/a.txt", "alpha"}, {"docs/sub/b.txt", "beta"}, {"top.txt", "top"},
    {".hidden", "h"}, {"\xc3\xbc.txt", "u"}, {"./dot/x.txt", "x"},
};
#define NSAMPLE ((int)(sizeof sample / sizeof *sample))

static void check_sample(const char *archive)
{
    char *r = in(archive, "");
    fr_entries es = {0};
    CHECK(fr_vfs_list(r, &es) == 0);
    CHECK(es.n == 5);
    CHECK(has(&es, "docs", 1) && has(&es, "top.txt", 0) && has(&es, ".hidden", 0) && has(&es, "\xc3\xbc.txt", 0));
    CHECK(has(&es, "dot", 1));
    for (int i = 0; i < es.n; i++)
        if (!strcmp(es.v[i].name, ".hidden")) CHECK(es.v[i].hidden && es.v[i].size == 1 && es.v[i].mtime == 1700000000);
    fr_entries_free(&es);
    char *docs = in(archive, "docs");
    CHECK(fr_vfs_list(docs, &es) == 0);
    CHECK(es.n == 2 && has(&es, "a.txt", 0) && has(&es, "sub", 1));
    fr_entries_free(&es);
    char *sub = in(archive, "docs/sub/");
    fr_entry e = {0};
    CHECK(fr_vfs_stat(sub, &e) == 0 && e.is_dir);
    CHECK_STR(e.name, "sub");
    fr_entry_free(&e);
    char *b = in(archive, "docs/sub/b.txt"), *u = in(archive, "\xc3\xbc.txt");
    check_read(b, "beta");
    check_read(u, "u");
    int err = 0;
    CHECK(fr_vfs_open_read(docs, &err) == NULL && err == -EISDIR);
    char *missing = in(archive, "nope");
    CHECK(fr_vfs_stat(missing, &e) == -ENOENT);
    CHECK(fr_vfs_list(b, &es) == -ENOTDIR);
    free(r), free(docs), free(sub), free(b), free(u), free(missing);
}

static void uris(void)
{
    const char *u = "archive://file:///a b/x.zip!/docs/sub";
    char *outer = fr_archive_outer(u), *inner = fr_archive_inner(u), *parent = fr_uri_parent(u), *name = fr_uri_name(u);
    CHECK_STR(outer, "file:///a b/x.zip");
    CHECK_STR(inner, "docs/sub");
    CHECK_STR(parent, "archive://file:///a b/x.zip!/docs");
    CHECK_STR(name, "sub");
    CHECK_STR(fr_uri_path(u), "/docs/sub");
    free(outer), free(inner), free(parent), free(name);
    const char *r = "archive://file:///a b/x.zip!/";
    parent = fr_uri_parent(r), name = fr_uri_name(r), inner = fr_archive_inner(r);
    CHECK_STR(parent, "file:///a b");
    CHECK_STR(name, "x.zip");
    CHECK_STR(inner, "");
    free(parent), free(name), free(inner);
    char *joined = fr_uri_join(r, "c.txt");
    CHECK_STR(joined, "archive://file:///a b/x.zip!/c.txt");
    free(joined);
    const char *nested = "archive://archive://file:///a.zip!/in/b.tar!/c/d";
    outer = fr_archive_outer(nested), inner = fr_archive_inner(nested);
    CHECK_STR(outer, "archive://file:///a.zip!/in/b.tar");
    CHECK_STR(inner, "c/d");
    free(outer), free(inner);
    CHECK(fr_uri_same_backend("archive://file:///a.zip!/x", "archive://file:///a.zip!/y/z"));
    CHECK(!fr_uri_same_backend("archive://file:///a.zip!/x", "archive://file:///b.zip!/x"));
    CHECK(!fr_uri_same_backend("archive://file:///a.zip!/x", "file:///a.zip"));
    CHECK(fr_uri_local(r) == NULL);
    CHECK(!fr_archive_is_uri("file:///a.zip"));
    CHECK(fr_archive_browsable("A.ZIP") && fr_archive_browsable("x.tar.gz") && fr_archive_browsable("x.7z"));
    CHECK(fr_archive_browsable("x.rar") && fr_archive_browsable("x.iso"));
    CHECK(!fr_archive_browsable("x.apk") && !fr_archive_browsable("x.docx") && !fr_archive_browsable("x.gz"));
    CHECK(fr_archive_writable("x.zip") && fr_archive_writable("x.tar.zst") && !fr_archive_writable("x.rar"));
}

/* single changes through the backend: each rewrites the archive */
static void edits(const char *archive)
{
    char *dir = in(archive, "new"), *file = in(archive, "new/c.txt"), *top = in(archive, "top.txt");
    char *renamed = in(archive, "renamed.txt"), *docs = in(archive, "docs"), *moved = in(archive, "new/docs");
    CHECK(fr_vfs_mkdir(dir) == 0);
    CHECK(fr_vfs_mkdir(dir) == -EEXIST);
    put(file, "gamma");
    check_read(file, "gamma");
    put(file, "gamma2");
    check_read(file, "gamma2");
    int err = 0;
    CHECK(fr_vfs_open_write(file, 1, &err) == NULL && err == -EEXIST);
    CHECK(fr_vfs_rename(top, renamed) == 0);
    CHECK(!exists(top) && exists(renamed));
    check_read(renamed, "top");
    CHECK(fr_vfs_rename(renamed, file) == -EEXIST);
    CHECK(fr_vfs_rename(docs, moved) == 0);
    char *b = in(archive, "new/docs/sub/b.txt");
    check_read(b, "beta");
    CHECK(fr_vfs_remove(dir) == -ENOTEMPTY);
    CHECK(fr_vfs_remove(renamed) == 0 && !exists(renamed));
    fr_entries es = {0};
    char *r = in(archive, "");
    CHECK(fr_vfs_list(r, &es) == 0);
    CHECK(es.n == 4 && has(&es, "new", 1) && !has(&es, "docs", 1));
    fr_entries_free(&es);
    char *orphan = in(archive, "no/such/c.txt");
    CHECK(fr_vfs_open_write(orphan, 0, &err) == NULL && err == -ENOENT);
    CHECK(fr_vfs_mkdir(orphan) == -ENOENT);
    /* no temporary file is left beside it */
    CHECK(fr_vfs_list(root, &es) == 0);
    for (int i = 0; i < es.n; i++) CHECK(!strstr(es.v[i].name, ".ferret-"));
    fr_entries_free(&es);
    free(dir), free(file), free(top), free(renamed), free(docs), free(moved), free(b), free(r), free(orphan);
}

static fr_job_status wait_job(int id)
{
    fr_jobs_wait_idle();
    fr_job_status st[8], r = {0};
    int k = fr_jobs_list(st, 8);
    for (int i = 0; i < k; i++)
        if (st[i].id == id) r = st[i];
    fr_job_dismiss(id);
    return r;
}

static void jobs(void)
{
    fr_jobs_start(NULL);
    char *j = at("j"), *jf = at("j/folder"), *jff = at("j/folder/f.txt"), *jl = at("j/loose.txt");
    CHECK(fr_vfs_mkdir(j) == 0);
    CHECK(fr_vfs_mkdir(jf) == 0);
    put(jff, "folder file");
    put(jl, "loose");
    free(j), free(jf), free(jff), free(jl);
    make_archive("j/box.zip", archive_write_set_format_zip, NULL, sample, NSAMPLE);

    /* copy into an archive: one rewrite */
    fr_strs src = {0};
    FR_PUSH(src, at("j/folder"));
    FR_PUSH(src, at("j/loose.txt"));
    char *docs = in("j/box.zip", "docs");
    fr_job_status st = wait_job(fr_job_submit(JOB_COPY, &src, docs));
    CHECK(st.state == JOB_DONE && st.failed == 0);
    CHECK(st.bytes_total > 0 && st.bytes_done >= st.bytes_total);
    char *f = in("j/box.zip", "docs/folder/f.txt"), *l = in("j/box.zip", "docs/loose.txt");
    check_read(f, "folder file");
    check_read(l, "loose");
    fr_strs_free(&src);

    /* again: the names are taken, so the copies get new ones */
    FR_PUSH(src, at("j/loose.txt"));
    int id = fr_job_submit(JOB_COPY, &src, docs);
    for (int tries = 0; tries < 5000; tries++) {
        fr_job_status s[8];
        int k = fr_jobs_list(s, 8), asking = 0;
        for (int i = 0; i < k; i++)
            if (s[i].id == id && s[i].state == JOB_ASKING) asking = 1;
        if (asking) {
            fr_job_answer(id, CONFLICT_KEEP_BOTH, 0);
            break;
        }
        usleep(1000);
    }
    st = wait_job(id);
    CHECK(st.failed == 0 && exists_in("j/box.zip", "docs/loose (2).txt"));
    fr_strs_free(&src);

    /* replacing a folder merges it: what only the archive has stays */
    char *only = in("j/box.zip", "docs/folder/only.txt"), *ff2 = at("j/folder/f.txt");
    put(only, "only");
    put(ff2, "folder file 2");
    FR_PUSH(src, at("j/folder"));
    id = fr_job_submit(JOB_COPY, &src, docs);
    for (int tries = 0; tries < 5000; tries++) {
        fr_job_status s[8];
        int k = fr_jobs_list(s, 8), asking = 0;
        for (int i = 0; i < k; i++)
            if (s[i].id == id && s[i].state == JOB_ASKING) asking = s[i].conflict_dir ? 2 : 1;
        if (asking) {
            CHECK(asking == 2);
            fr_job_answer(id, CONFLICT_REPLACE, 0);
            break;
        }
        usleep(1000);
    }
    st = wait_job(id);
    CHECK(st.failed == 0);
    check_read(only, "only");
    check_read(f, "folder file 2");
    put(ff2, "folder file");
    free(only), free(ff2);
    fr_strs_free(&src);

    /* an archive into itself */
    FR_PUSH(src, at("j/box.zip"));
    st = wait_job(fr_job_submit(JOB_MOVE, &src, docs));
    CHECK(st.failed == 1 && exists_in("j/box.zip", "docs/a.txt"));
    fr_strs_free(&src);

    /* delete inside */
    FR_PUSH(src, in("j/box.zip", "docs/loose (2).txt"));
    FR_PUSH(src, in("j/box.zip", "top.txt"));
    st = wait_job(fr_job_submit(JOB_DELETE, &src, NULL));
    CHECK(st.failed == 0 && !exists_in("j/box.zip", "docs/loose (2).txt") && !exists_in("j/box.zip", "top.txt"));
    CHECK(exists_in("j/box.zip", "docs/a.txt"));
    fr_strs_free(&src);

    /* copy out of an archive */
    FR_PUSH(src, in("j/box.zip", "docs"));
    char *out = at("j/out");
    CHECK(fr_vfs_mkdir(out) == 0);
    st = wait_job(fr_job_submit(JOB_COPY, &src, out));
    CHECK(st.state == JOB_DONE && st.failed == 0);
    char *b = at("j/out/docs/sub/b.txt"), *ff = at("j/out/docs/folder/f.txt");
    check_read(b, "beta");
    check_read(ff, "folder file 2");
    fr_strs_free(&src);

    /* move within the archive, then out of it */
    FR_PUSH(src, in("j/box.zip", "docs/folder"));
    char *r = in("j/box.zip", "");
    st = wait_job(fr_job_submit(JOB_MOVE, &src, r));
    CHECK(st.failed == 0 && exists_in("j/box.zip", "folder/f.txt") && !exists_in("j/box.zip", "docs/folder"));
    fr_strs_free(&src);
    FR_PUSH(src, in("j/box.zip", "folder"));
    char *moved_out = at("j/moved");
    CHECK(fr_vfs_mkdir(moved_out) == 0);
    st = wait_job(fr_job_submit(JOB_MOVE, &src, moved_out));
    CHECK(st.failed == 0 && !exists_in("j/box.zip", "folder"));
    char *mf = at("j/moved/folder/f.txt");
    check_read(mf, "folder file 2");
    fr_strs_free(&src);

    /* extract here and into a folder of its own */
    FR_PUSH(src, at("j/box.zip"));
    char *x = at("j/x");
    CHECK(fr_vfs_mkdir(x) == 0);
    st = wait_job(fr_job_submit_extract(&src, x, 0));
    CHECK(st.state == JOB_DONE && st.failed == 0);
    char *xa = at("j/x/docs/a.txt");
    check_read(xa, "alpha");
    st = wait_job(fr_job_submit_extract(&src, x, 1));
    CHECK(st.failed == 0);
    char *xb = at("j/x/box/docs/sub/b.txt");
    check_read(xb, "beta");
    st = wait_job(fr_job_submit_extract(&src, x, 1));
    char *xc = at("j/x/box (2)/top.txt");
    CHECK(!exists(xc));
    free(xc);
    xc = at("j/x/box (2)/docs/a.txt");
    check_read(xc, "alpha");
    fr_strs_free(&src);

    /* compress, with a symlink, which comes back out as one */
    char *link = fr_format("%s/j/folder/link", root_path);
    CHECK(symlink("f.txt", link) == 0);
    FR_PUSH(src, at("j/folder"));
    FR_PUSH(src, at("j/loose.txt"));
    char *zip = at("j/packed.tar.gz");
    st = wait_job(fr_job_submit_compress(&src, zip));
    CHECK(st.state == JOB_DONE && st.failed == 0);
    char *pf = in("j/packed.tar.gz", "folder/f.txt"), *pl = in("j/packed.tar.gz", "loose.txt");
    char *plink = in("j/packed.tar.gz", "folder/link");
    check_read(pf, "folder file");
    check_read(pl, "loose");
    fr_entry le = {0};
    CHECK(fr_vfs_stat(plink, &le) == 0 && le.is_link);
    fr_entry_free(&le);
    st = wait_job(fr_job_submit_compress(&src, zip));
    CHECK(st.failed == 1);
    fr_strs_free(&src);
    FR_PUSH(src, at("j/packed.tar.gz"));
    st = wait_job(fr_job_submit_extract(&src, x, 1));
    CHECK(st.failed == 0);
    char *xlink = fr_format("%s/j/x/packed/folder/link", root_path), target[64] = "";
    ssize_t tn = readlink(xlink, target, sizeof target - 1);
    CHECK(tn == 5 && !strncmp(target, "f.txt", 5));
    fr_strs_free(&src);
    free(link), free(plink), free(xlink);

    /* several archives as one job, each into a folder of its own, a name taken there kept */
    char *batch = at("j/batch"), *taken = at("j/batch/box");
    CHECK(fr_vfs_mkdir(batch) == 0 && fr_vfs_mkdir(taken) == 0);
    FR_PUSH(src, at("j/box.zip"));
    FR_PUSH(src, at("j/packed.tar.gz"));
    st = wait_job(fr_job_submit_extract(&src, batch, 1));
    CHECK(st.state == JOB_DONE && st.failed == 0 && st.items == 2);
    CHECK(st.bytes_total > 0 && st.bytes_done == st.bytes_total);
    char *ba = at("j/batch/box (2)/docs/a.txt"), *bl = at("j/batch/packed/loose.txt");
    check_read(ba, "alpha");
    check_read(bl, "loose");
    fr_entries left = {0};
    CHECK(fr_vfs_list(taken, &left) == 0 && left.n == 0);
    fr_entries_free(&left);
    fr_strs_free(&src);
    free(batch), free(taken), free(ba), free(bl);

    /* rename and new folder inside, as jobs */
    char *from = in("j/box.zip", "docs/a.txt"), *to = in("j/box.zip", "docs/A.txt"), *nf = in("j/box.zip", "made");
    st = wait_job(fr_job_submit_rename(from, to));
    CHECK(st.failed == 0 && !exists(from) && exists(to));
    st = wait_job(fr_job_submit_create(nf, 1));
    CHECK(st.failed == 0);
    fr_entry e = {0};
    CHECK(fr_vfs_stat(nf, &e) == 0 && e.is_dir);
    fr_entry_free(&e);

    fr_jobs_stop();
    free(docs), free(f), free(l), free(out), free(b), free(ff), free(r), free(moved_out), free(mf), free(x), free(xa);
    free(xb), free(xc), free(zip), free(pf), free(pl), free(from), free(to), free(nf);
}

static void remove_tree(const char *uri)
{
    fr_entry e = {0};
    if (fr_vfs_stat(uri, &e)) return;
    if (e.is_dir && !e.is_link) {
        fr_entries kids = {0};
        fr_vfs_list(uri, &kids);
        for (int i = 0; i < kids.n; i++) {
            char *c = fr_uri_join(uri, kids.v[i].name);
            remove_tree(c);
            free(c);
        }
        fr_entries_free(&kids);
    }
    fr_vfs_remove(uri);
    fr_entry_free(&e);
}

int main(void)
{
    char tmpl[] = "/tmp/ferret-archive-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    root_path = tmpl;
    root = fr_uri_from_path(tmpl);

    uris();

    make_archive("s.zip", archive_write_set_format_zip, NULL, sample, NSAMPLE);
    make_archive("s.tar.gz", archive_write_set_format_pax_restricted, archive_write_add_filter_gzip, sample, NSAMPLE);
    make_archive("s.7z", archive_write_set_format_7zip, NULL, sample, NSAMPLE);
    check_sample("s.zip");
    check_sample("s.tar.gz");
    check_sample("s.7z");
    /* read again from the cache, and after it was dropped */
    check_sample("s.zip");
    fr_archive_cache_clear();
    check_sample("s.zip");

    edits("s.zip");
    edits("s.tar.gz");
    edits("s.7z");

    /* cancelled: the archive stays as it was, and nothing is left beside it */
    int stop = 1;
    fr_archive_hooks cancel = {.cancel = &stop};
    char *sz = at("s.zip");
    fr_archive_change del = {.op = FR_AR_DELETE, .path = ".hidden"};
    CHECK(fr_archive_rewrite(sz, &del, 1, &cancel) == -ECANCELED);
    CHECK(exists_in("s.zip", ".hidden"));
    fr_entries left = {0};
    CHECK(fr_vfs_list(root, &left) == 0);
    for (int i = 0; i < left.n; i++) CHECK(!strstr(left.v[i].name, ".ferret-"));
    fr_entries_free(&left);
    free(sz);

    /* written through a symlink: the file it points to changes, the link stays */
    char *lpath = fr_format("%s/link.zip", root_path);
    CHECK(symlink("s.zip", lpath) == 0);
    char *ldir = in("link.zip", "via-link");
    CHECK(fr_vfs_mkdir(ldir) == 0);
    struct stat lst;
    CHECK(lstat(lpath, &lst) == 0 && S_ISLNK(lst.st_mode));
    CHECK(exists_in("s.zip", "via-link"));
    free(lpath), free(ldir);

    /* an archive in an archive */
    char *inner_path = fr_format("%s/s.tar.gz", root_path);
    {
        char *outer = fr_format("%s/nest.zip", root_path);
        struct archive *a = archive_write_new();
        archive_write_set_format_zip(a);
        archive_write_open_filename(a, outer);
        struct stat sb;
        stat(inner_path, &sb);
        struct archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, "in/s.tar.gz");
        archive_entry_set_filetype(e, AE_IFREG);
        archive_entry_set_perm(e, 0644);
        archive_entry_set_size(e, sb.st_size);
        archive_write_header(a, e);
        FILE *fp = fopen(inner_path, "rb");
        char buf[4096];
        size_t n;
        while (fp && (n = fread(buf, 1, sizeof buf, fp)) > 0) archive_write_data(a, buf, n);
        if (fp) fclose(fp);
        archive_entry_free(e);
        archive_write_close(a);
        archive_write_free(a);
        free(outer);
    }
    char *nested_outer = in("nest.zip", "in/s.tar.gz"), *nested_root = fr_archive_root(nested_outer);
    char *nested_file = fr_format("%snew/docs/sub/b.txt", nested_root), *nested_dir = fr_format("%sz", nested_root);
    check_read(nested_file, "beta");
    CHECK(fr_vfs_mkdir(nested_dir) == -ENOTSUP);
    free(nested_outer), free(nested_root), free(nested_file), free(nested_dir), free(inner_path);

    /* not an archive; a kind that is only read */
    char *text = at("plain.zip");
    put(text, "hello, this is not a zip file at all\n");
    char *tr = in("plain.zip", "");
    fr_entries es = {0};
    CHECK(fr_vfs_list(tr, &es) == -EBADMSG);
    fr_entries_free(&es);
    make_archive("s.iso", archive_write_set_format_iso9660, NULL, sample, 3);
    char *iso_dir = in("s.iso", "docs"), *iso_new = in("s.iso", "new");
    CHECK(fr_vfs_list(iso_dir, &es) == 0 && has(&es, "a.txt", 0));
    fr_entries_free(&es);
    CHECK(fr_vfs_mkdir(iso_new) == -ENOTSUP);
    free(text), free(tr), free(iso_dir), free(iso_new);

    /* an empty archive, made and then written into */
    fr_strs none = {0};
    char *empty = at("empty.zip"), *er = in("empty.zip", ""), *ed = in("empty.zip", "d");
    CHECK(fr_archive_create(empty, &none, NULL) == 0);
    CHECK(fr_vfs_list(er, &es) == 0 && es.n == 0);
    fr_entries_free(&es);
    CHECK(fr_vfs_mkdir(ed) == 0 && exists(ed));
    free(empty), free(er), free(ed);

    jobs();

    remove_tree(root);
    CHECK(!exists(root));
    free(root);
    return CHECK_DONE();
}

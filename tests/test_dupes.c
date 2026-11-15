#define _GNU_SOURCE
#include "check.h"
#include "jobs/async.h"
#include "jobs/dirsize.h"
#include "jobs/dupes.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

static char *root, *root_path;

static void put(const char *rel, const void *data, size_t n)
{
    char *uri = fr_format("%s/%s", root, rel);
    int err = 0;
    fr_stream *s = fr_vfs_open_write(uri, 0, &err);
    CHECK(s != NULL);
    if (s) {
        CHECK(s->write(s, data, n) == (long)n);
        s->close(s);
    }
    free(uri);
}

static void mk(const char *rel)
{
    char *uri = fr_format("%s/%s", root, rel);
    CHECK(fr_vfs_mkdir(uri) == 0);
    free(uri);
}

/* "group:path ..." in the order found */
static char *listed(const fr_dupes *d)
{
    char *out = fr_strdup("");
    for (int i = 0; i < d->files.n; i++) {
        char *next = fr_format("%s%s%d:%s", out, i ? " " : "", d->group[i], d->files.v[i].name);
        free(out);
        out = next;
    }
    return out;
}

static void remove_tree(const char *uri)
{
    fr_entry e = {0};
    if (fr_vfs_stat(uri, &e)) return;
    if (e.is_dir && !e.is_link) {
        fr_entries es = {0};
        fr_vfs_list(uri, &es);
        for (int i = 0; i < es.n; i++) {
            char *c = fr_uri_join(uri, es.v[i].name);
            remove_tree(c);
            free(c);
        }
        fr_entries_free(&es);
    }
    fr_entry_free(&e);
    fr_vfs_remove(uri);
}

#define BIG (100 * 1024)

static void dupes(void)
{
    mk("sub");
    mk("sub/deep");
    mk(".dot");
    put("a.txt", "hello", 5);
    put("sub/a copy.txt", "hello", 5);
    put("sub/deep/a3.txt", "hello", 5);
    put("b.txt", "world", 5);
    put(".hidden.txt", "hello", 5);
    put(".dot/inside.txt", "hello", 5);
    put("empty1", "", 0);
    put("sub/empty2", "", 0);
    unsigned char *big = malloc(BIG);
    for (int i = 0; i < BIG; i++) big[i] = (unsigned char)(i * 7 + i / 300);
    put("big1.bin", big, BIG);
    put("sub/big2.bin", big, BIG);
    big[BIG - 1] ^= 1; /* the same first 64 KB, another end */
    put("sub/big3.bin", big, BIG);
    free(big);
    char *target = fr_format("%s/a.txt", root_path), *link = fr_format("%s/link.txt", root_path);
    CHECK(symlink(target, link) == 0);
    free(target), free(link);

    fr_dupes d;
    fr_dupes_status st = {0};
    CHECK(fr_dupes_find(root, 0, &d, &st, NULL) == 0);
    char *got = listed(&d);
    CHECK_STR(got, "1:big1.bin 1:sub/big2.bin 2:a.txt 2:sub/a copy.txt 2:sub/deep/a3.txt");
    free(got);
    CHECK(d.ngroups == 2);
    CHECK(d.wasted == BIG + 2 * 5);
    CHECK(st.phase == FR_DUPES_DONE);
    CHECK(st.files == 9 && st.folders == 3);
    CHECK(st.hash_total == 3 * (uint64_t)BIG + 4 * 5);
    CHECK(st.hash_done == st.hash_total);
    CHECK(d.files.v[0].size == BIG);
    fr_dupes_free(&d);

    CHECK(fr_dupes_find(root, 1, &d, NULL, NULL) == 0);
    got = listed(&d);
    CHECK_STR(got, "1:big1.bin 1:sub/big2.bin 2:.dot/inside.txt 2:.hidden.txt 2:a.txt 2:sub/a copy.txt 2:sub/deep/a3.txt");
    free(got);
    fr_dupes_free(&d);

    int cancel = 1;
    CHECK(fr_dupes_find(root, 0, &d, NULL, &cancel) == -ECANCELED);
    CHECK(d.files.n == 0 && d.ngroups == 0);
    char *gone = fr_format("%s/nothing", root);
    CHECK(fr_dupes_find(gone, 0, &d, NULL, NULL) == -ENOENT);
    free(gone);
}

static void sizes(void)
{
    fr_strs uris = {0};
    FR_PUSH(uris, fr_format("%s/sub", root));
    FR_PUSH(uris, fr_format("%s/.dot", root));
    FR_PUSH(uris, fr_format("%s/missing", root));
    fr_dirsize *d = fr_dirsize_new(&uris);
    fr_async_start(NULL);
    fr_async_run(fr_dirsize_work, (void (*)(void *))fr_dirsize_free, d);
    fr_async_wait();
    CHECK(d->finished == 3);
    /* sub: a copy.txt, empty2, big2.bin, big3.bin, deep/a3.txt */
    CHECK(d->counts[0].bytes == 5 + 2 * (uint64_t)BIG + 5);
    CHECK(d->counts[0].files == 5 && d->counts[0].folders == 2);
    CHECK(d->counts[1].bytes == 5 && d->counts[1].files == 1);
    CHECK(d->counts[2].unreadable == 1 && d->counts[2].bytes == 0);
    CHECK(fr_async_poll() == 1);

    d = fr_dirsize_new(&uris);
    d->cancel = 1;
    fr_dirsize_work(d);
    CHECK(d->finished == 0 && d->counts[0].bytes == 0);
    fr_dirsize_free(d);
    fr_strs_free(&uris);
}

int main(void)
{
    char tmpl[] = "/tmp/ferret-dupes-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    root_path = tmpl;
    root = fr_uri_from_path(tmpl);
    dupes();
    sizes();
    remove_tree(root);
    free(root);
    return CHECK_DONE();
}

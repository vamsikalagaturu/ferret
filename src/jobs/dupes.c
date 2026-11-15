#include "dupes.h"
#include "core/hash.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    fr_entry e; /* name: the path below the root */
    char pre[65], full[65];
    int unread;
} cand;

typedef struct {
    cand *v;
    int n, cap;
} cands;

static int cancelled(const int *cancel) { return cancel && __atomic_load_n(cancel, __ATOMIC_RELAXED); }

static void add64(int64_t *f, int64_t n) { __atomic_add_fetch(f, n, __ATOMIC_RELAXED); }
static void addu(uint64_t *f, uint64_t n) { __atomic_add_fetch(f, n, __ATOMIC_RELAXED); }

static int walk(const char *uri, const char *rel, int hidden, cands *out, fr_dupes_status *st, const int *cancel)
{
    fr_entries es = {0};
    int rc = fr_vfs_list(uri, &es);
    add64(&st->folders, 1);
    if (rc) {
        add64(&st->unreadable, 1);
        fr_entries_free(&es);
        return rc;
    }
    for (int i = 0; i < es.n && !cancelled(cancel); i++) {
        fr_entry *e = &es.v[i];
        if (e->is_link || (e->hidden && !hidden)) continue;
        char *path = *rel ? fr_format("%s/%s", rel, e->name) : fr_strdup(e->name);
        if (e->is_dir) {
            char *child = fr_uri_join(uri, e->name);
            walk(child, path, hidden, out, st, cancel);
            free(child);
            free(path);
            continue;
        }
        add64(&st->files, 1);
        if (!e->size) {
            free(path);
            continue;
        }
        free(e->name);
        e->name = path;
        FR_PUSH(*out, ((cand){.e = *e}));
        *e = (fr_entry){0};
    }
    fr_entries_free(&es);
    return 0;
}

/* SHA-256 of the first limit bytes (0: all) into hex; counts what it read past skip into hash_done */
static int hash_file(const char *uri, uint64_t limit, uint64_t skip, char hex[65], fr_dupes_status *st, const int *cancel)
{
    int err = 0;
    fr_stream *s = fr_vfs_open_read(uri, &err);
    if (!s) return err ? err : -EIO;
    fr_hash h;
    fr_hash_init(&h, FR_SHA256);
    enum { CHUNK = 64 * 1024 };
    char *buf = fr_realloc(NULL, CHUNK);
    uint64_t at = 0;
    long got = 0;
    while (!limit || at < limit) {
        if (cancelled(cancel)) {
            got = -ECANCELED;
            break;
        }
        size_t want = limit && limit - at < CHUNK ? (size_t)(limit - at) : CHUNK;
        if ((got = s->read(s, buf, want)) <= 0) break;
        fr_hash_update(&h, buf, (size_t)got);
        uint64_t from = at;
        at += (uint64_t)got;
        if (at > skip) addu(&st->hash_done, at - (from > skip ? from : skip));
    }
    free(buf);
    s->close(s);
    if (got < 0) return (int)got;
    fr_hash_final_hex(&h, hex);
    return 0;
}

static int by_size(const void *pa, const void *pb)
{
    const cand *a = pa, *b = pb;
    return a->e.size < b->e.size ? 1 : a->e.size > b->e.size ? -1 : 0;
}

static int by_pre(const void *pa, const void *pb)
{
    const cand *a = pa, *b = pb;
    if (a->unread != b->unread) return a->unread - b->unread;
    int c = strcmp(a->pre, b->pre);
    return c ? c : fr_natcmp(a->e.name, b->e.name);
}

static int by_full(const void *pa, const void *pb)
{
    const cand *a = pa, *b = pb;
    if (a->unread != b->unread) return a->unread - b->unread;
    int c = strcmp(a->full, b->full);
    return c ? c : fr_natcmp(a->e.name, b->e.name);
}

static int by_path(const void *pa, const void *pb)
{
    const cand *a = pa, *b = pb;
    int c = fr_natcmp(a->e.name, b->e.name);
    return c ? c : strcmp(a->e.name, b->e.name);
}

/* the run [from, to) of alike files becomes the next group of out */
static void emit(cand *v, int from, int to, fr_dupes *out)
{
    qsort(v + from, (size_t)(to - from), sizeof *v, by_path);
    out->ngroups++;
    for (int i = from; i < to; i++) {
        if (i > from) out->wasted += v[i].e.size;
        FR_PUSH(out->files, v[i].e);
        v[i].e = (fr_entry){0};
        out->group = fr_realloc(out->group, sizeof *out->group * (size_t)out->files.cap);
        out->group[out->files.n - 1] = out->ngroups;
    }
}

/* v[0..n) share a size: grouped by content into out */
static void sift(const char *root, cand *v, int n, fr_dupes *out, fr_dupes_status *st, const int *cancel)
{
    uint64_t size = v[0].e.size;
    for (int i = 0; i < n && !cancelled(cancel); i++) {
        char *uri = fr_uri_join(root, v[i].e.name);
        if (hash_file(uri, FR_DUPES_PREFIX, 0, v[i].pre, st, cancel)) v[i].unread = 1, add64(&st->unreadable, 1);
        free(uri);
    }
    if (cancelled(cancel)) return;
    qsort(v, (size_t)n, sizeof *v, by_pre);
    for (int a = 0; a < n;) {
        int b = a + 1;
        while (b < n && !v[a].unread && !v[b].unread && !strcmp(v[a].pre, v[b].pre)) b++;
        if (v[a].unread || b - a < 2) {
            if (!v[a].unread && size > FR_DUPES_PREFIX) addu(&st->hash_done, size - FR_DUPES_PREFIX);
            a++;
            continue;
        }
        if (size <= FR_DUPES_PREFIX) {
            emit(v, a, b, out);
            a = b;
            continue;
        }
        for (int i = a; i < b && !cancelled(cancel); i++) {
            char *uri = fr_uri_join(root, v[i].e.name);
            if (hash_file(uri, 0, FR_DUPES_PREFIX, v[i].full, st, cancel)) v[i].unread = 1, add64(&st->unreadable, 1);
            free(uri);
        }
        if (cancelled(cancel)) return;
        qsort(v + a, (size_t)(b - a), sizeof *v, by_full);
        for (int c = a; c < b;) {
            int d = c + 1;
            while (d < b && !v[c].unread && !v[d].unread && !strcmp(v[c].full, v[d].full)) d++;
            if (!v[c].unread && d - c >= 2) emit(v, c, d, out);
            c = d;
        }
        a = b;
    }
}

int fr_dupes_find(const char *root, int hidden, fr_dupes *out, fr_dupes_status *st, const int *cancel)
{
    *out = (fr_dupes){0};
    fr_dupes_status none = {0};
    if (!st) st = &none;
    cands all = {0};
    int rc = walk(root, "", hidden, &all, st, cancel);
    if (!rc) {
        if (all.n > 1) qsort(all.v, (size_t)all.n, sizeof *all.v, by_size);
        uint64_t total = 0;
        for (int a = 0, b; a < all.n; a = b) {
            for (b = a + 1; b < all.n && all.v[b].e.size == all.v[a].e.size; b++) {}
            if (b - a >= 2) total += all.v[a].e.size * (uint64_t)(b - a);
        }
        __atomic_store_n(&st->hash_total, total, __ATOMIC_RELAXED);
        __atomic_store_n(&st->phase, FR_DUPES_HASHING, __ATOMIC_RELAXED);
        for (int a = 0, b; a < all.n && !cancelled(cancel); a = b) {
            for (b = a + 1; b < all.n && all.v[b].e.size == all.v[a].e.size; b++) {}
            if (b - a >= 2) sift(root, all.v + a, b - a, out, st, cancel);
        }
    }
    for (int i = 0; i < all.n; i++) fr_entry_free(&all.v[i].e);
    free(all.v);
    if (!rc && cancelled(cancel)) rc = -ECANCELED;
    if (rc) fr_dupes_free(out);
    __atomic_store_n(&st->phase, FR_DUPES_DONE, __ATOMIC_RELEASE);
    return rc;
}

void fr_dupes_free(fr_dupes *d)
{
    fr_entries_free(&d->files);
    free(d->group);
    *d = (fr_dupes){0};
}

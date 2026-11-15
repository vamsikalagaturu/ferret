#include "compare.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

fr_cmp_state fr_cmp_classify(const fr_entry *a, const fr_entry *b, int tolerance, int same_content)
{
    if (!a || !a->name) return CMP_NONE;
    if (!b || !b->name) return CMP_ONLY;
    int a_dir = a->is_dir && !a->is_link, b_dir = b->is_dir && !b->is_link;
    if (a_dir && b_dir) return CMP_FOLDER;
    if (a_dir != b_dir || a->is_link != b->is_link) return CMP_DIFFERENT;
    int64_t dt = a->mtime - b->mtime;
    int same_time = dt <= tolerance && dt >= -tolerance;
    if (a->size == b->size && (same_content == 1 || (same_content < 0 && same_time))) return CMP_SAME;
    if (!same_time) return dt > 0 ? CMP_NEWER : CMP_OLDER;
    return CMP_DIFFERENT;
}

static fr_entry entry_dup(const fr_entry *e)
{
    fr_entry c = *e;
    c.name = e->name ? fr_strdup(e->name) : NULL;
    return c;
}

/* fills buf from s as far as it goes: the bytes read, or -errno */
static long read_full(fr_stream *s, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        long r = s->read(s, buf + got, n - got);
        if (r < 0) return r;
        if (!r) break;
        got += (size_t)r;
    }
    return (long)got;
}

/* 1 alike, 0 not, -1 one could not be read */
static int same_bytes(const char *a, const char *b, const int *cancel)
{
    int err = 0;
    fr_stream *sa = fr_vfs_open_read(a, &err), *sb = sa ? fr_vfs_open_read(b, &err) : NULL;
    int rc = sa && sb ? 1 : -1;
    enum { CHUNK = 64 * 1024 };
    char *ba = rc > 0 ? fr_realloc(NULL, 2 * CHUNK) : NULL, *bb = ba ? ba + CHUNK : NULL;
    while (rc == 1) {
        if (__atomic_load_n(cancel, __ATOMIC_RELAXED)) {
            rc = -1;
            break;
        }
        long na = read_full(sa, ba, CHUNK), nb = read_full(sb, bb, CHUNK);
        if (na < 0 || nb < 0) rc = -1;
        else if (na != nb || memcmp(ba, bb, (size_t)na)) rc = 0;
        else if (!na) break;
    }
    free(ba);
    if (sa) sa->close(sa);
    if (sb) sb->close(sb);
    return rc;
}

static int by_name(const void *pa, const void *pb) { return strcmp(((const fr_entry *)pa)->name, ((const fr_entry *)pb)->name); }

typedef struct {
    const fr_cmp_options *o;
    fr_cmp_items *out;
    int64_t *scanned;
    const int *cancel;
} walk;

static char *child_path(const char *prefix, const char *name) { return *prefix ? fr_format("%s/%s", prefix, name) : fr_strdup(name); }

/* the items of lu and ru below prefix into w->out; 1 when anything differs */
static int compare_dir(walk *w, const char *lu, const char *ru, const char *prefix, int *err)
{
    fr_entries le = {0}, re = {0};
    int rl = fr_vfs_list(lu, &le), rr = fr_vfs_list(ru, &re);
    if (rl || rr) {
        if (err) *err = rl ? rl : rr;
        fr_entries_free(&le), fr_entries_free(&re);
        return 1;
    }
    qsort(le.v, (size_t)le.n, sizeof *le.v, by_name);
    qsort(re.v, (size_t)re.n, sizeof *re.v, by_name);
    int first = w->out->n, differs = 0;
    for (int i = 0, k = 0; i < le.n || k < re.n;) {
        int c = i >= le.n ? 1 : k >= re.n ? -1 : strcmp(le.v[i].name, re.v[k].name);
        const fr_entry *l = c <= 0 ? &le.v[i++] : NULL, *r = c >= 0 ? &re.v[k++] : NULL;
        __atomic_add_fetch(w->scanned, 1, __ATOMIC_RELAXED);
        int same = -1;
        const char *name = l ? l->name : r->name;
        if (w->o->content && l && r && !l->is_dir && !r->is_dir && !l->is_link && !r->is_link && l->size == r->size) {
            char *a = fr_uri_join(lu, name), *b = fr_uri_join(ru, name);
            same = same_bytes(a, b, w->cancel);
            free(a), free(b);
        }
        fr_cmp_item it = {.path = child_path(prefix, name), .left = fr_cmp_classify(l, r, w->o->tolerance, same),
                          .right = fr_cmp_classify(r, l, w->o->tolerance, same)};
        if (l) it.l = entry_dup(l);
        if (r) it.r = entry_dup(r);
        FR_PUSH(*w->out, it);
    }
    int last = w->out->n;
    for (int i = first; i < last; i++) {
        fr_cmp_item *it = &w->out->v[i];
        if (it->left == CMP_FOLDER && w->o->recursive && !__atomic_load_n(w->cancel, __ATOMIC_RELAXED)) {
            char *a = fr_uri_join(lu, it->l.name), *b = fr_uri_join(ru, it->l.name), *p = fr_strdup(it->path);
            int inside = compare_dir(w, a, b, p, NULL);
            free(a), free(b), free(p);
            it = &w->out->v[i];
            it->left = it->right = inside ? CMP_INSIDE : CMP_SAME;
        }
        if (it->left != CMP_SAME) differs = 1;
    }
    /* below the top only what differs is kept */
    if (*prefix) {
        int kept = first;
        for (int i = first; i < w->out->n; i++) {
            fr_cmp_item *it = &w->out->v[i];
            if (i < last && it->left == CMP_SAME) {
                free(it->path), fr_entry_free(&it->l), fr_entry_free(&it->r);
                continue;
            }
            w->out->v[kept++] = *it;
        }
        w->out->n = kept;
    }
    fr_entries_free(&le), fr_entries_free(&re);
    return differs;
}

int fr_cmp_folders(const char *left, const char *right, const fr_cmp_options *o, fr_cmp_items *out, int64_t *scanned,
                   const int *cancel)
{
    walk w = {o, out, scanned, cancel};
    int err = 0;
    /* the top level first, so out->v[0..top) is it */
    fr_cmp_options top = *o;
    top.recursive = 0;
    w.o = &top;
    compare_dir(&w, left, right, "", &err);
    out->top = out->n;
    if (err || !o->recursive) return err;
    w.o = o;
    for (int i = 0; i < out->top && !__atomic_load_n(cancel, __ATOMIC_RELAXED); i++) {
        if (out->v[i].left != CMP_FOLDER) continue;
        char *a = fr_uri_join(left, out->v[i].path), *b = fr_uri_join(right, out->v[i].path), *p = fr_strdup(out->v[i].path);
        int inside = compare_dir(&w, a, b, p, NULL);
        free(a), free(b), free(p);
        out->v[i].left = out->v[i].right = inside ? CMP_INSIDE : CMP_SAME;
    }
    return __atomic_load_n(cancel, __ATOMIC_RELAXED) ? -ECANCELED : 0;
}

void fr_cmp_items_free(fr_cmp_items *items)
{
    for (int i = 0; i < items->n; i++) free(items->v[i].path), fr_entry_free(&items->v[i].l), fr_entry_free(&items->v[i].r);
    free(items->v);
    *items = (fr_cmp_items){0};
}

const fr_cmp_item *fr_cmp_find(const fr_cmp_items *items, const char *name)
{
    int lo = 0, hi = items->top - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(items->v[mid].path, name);
        if (!c) return &items->v[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

fr_steps fr_sync_plan(const fr_cmp_items *items, const char *left, const char *right, int to_right, int mirror,
                      fr_sync_counts *counts)
{
    fr_steps s = {0};
    fr_sync_counts c = {0};
    const char *from = to_right ? left : right, *to = to_right ? right : left;
    for (int i = 0; i < items->n; i++) {
        const fr_cmp_item *it = &items->v[i];
        fr_cmp_state here = to_right ? it->left : it->right;
        const fr_entry *src = to_right ? &it->l : &it->r;
        fr_step_kind kind;
        if (here == CMP_ONLY) kind = STEP_COPIED, c.copy++;
        else if (here == CMP_NEWER || (mirror && (here == CMP_OLDER || here == CMP_DIFFERENT))) kind = STEP_REPLACED, c.replace++;
        else if (here == CMP_OLDER || here == CMP_DIFFERENT) {
            c.skipped++;
            continue;
        } else if (here == CMP_NONE) {
            c.extra++;
            if (!mirror) continue;
            kind = STEP_TRASH, c.remove++;
        } else continue;
        char *a = fr_uri_join(from, it->path), *b = fr_uri_join(to, it->path);
        if (kind == STEP_TRASH) fr_steps_add(&s, STEP_TRASH, b, NULL);
        else fr_steps_add(&s, kind, a, b);
        const fr_entry *e = kind == STEP_TRASH ? (to_right ? &it->r : &it->l) : src;
        s.v[s.n - 1].is_dir = e->is_dir && !e->is_link;
        if (kind != STEP_TRASH && !s.v[s.n - 1].is_dir) c.bytes += src->size;
        free(a), free(b);
    }
    if (counts) *counts = c;
    return s;
}

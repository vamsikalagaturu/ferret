#define _GNU_SOURCE
#include "vfs_archive.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define PREFIX "archive://"
#define PREFIX_LEN (sizeof PREFIX - 1)
#define BUF_SIZE (256 * 1024)

/* ---- URIs and kinds ---- */

const char *fr_archive_sep(const char *uri)
{
    int depth = 0;
    const char *p = uri;
    while (!strncmp(p, PREFIX, PREFIX_LEN)) p += PREFIX_LEN, depth++;
    if (!depth) return NULL;
    for (const char *s = p; (s = strchr(s, '!')); s++)
        if ((s[1] == '/' || !s[1]) && !--depth) return s;
    return NULL;
}

int fr_archive_is_uri(const char *uri) { return fr_archive_sep(uri) != NULL; }

char *fr_archive_outer(const char *uri)
{
    const char *s = fr_archive_sep(uri);
    return s ? fr_strndup(uri + PREFIX_LEN, (size_t)(s - uri) - PREFIX_LEN) : NULL;
}

/* "/a//b/" -> "a/b" */
static char *clean_path(const char *p)
{
    char *out = fr_realloc(NULL, strlen(p) + 1), *o = out;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        if (o != out) *o++ = '/';
        while (*p && *p != '/') *o++ = *p++;
    }
    *o = 0;
    return out;
}

char *fr_archive_inner(const char *uri)
{
    const char *s = fr_archive_sep(uri);
    return s ? clean_path(s + 1) : NULL;
}

char *fr_archive_root(const char *outer) { return fr_format(PREFIX "%s!/", outer); }

static int ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    return n > k && !strcasecmp(s + n - k, suffix);
}

static const char *const browsable[] = {".zip", ".7z", ".rar", ".iso", ".cpio", ".tar", ".tgz", ".tar.gz", ".tbz",
                                        ".tbz2", ".tar.bz2", ".txz", ".tar.xz", ".tzst", ".tar.zst", ".tar.lzma"};

int fr_archive_browsable(const char *name)
{
    for (size_t i = 0; i < sizeof browsable / sizeof *browsable; i++)
        if (ends_with(name, browsable[i])) return 1;
    return 0;
}

static const struct {
    const char *ext;
    int (*filter)(struct archive *);
} written[] = {
    {".zip", NULL},
    {".7z", NULL},
    {".tar", NULL},
    {".tar.gz", archive_write_add_filter_gzip},
    {".tgz", archive_write_add_filter_gzip},
    {".tar.bz2", archive_write_add_filter_bzip2},
    {".tbz", archive_write_add_filter_bzip2},
    {".tbz2", archive_write_add_filter_bzip2},
    {".tar.xz", archive_write_add_filter_xz},
    {".txz", archive_write_add_filter_xz},
    {".tar.zst", archive_write_add_filter_zstd},
    {".tzst", archive_write_add_filter_zstd},
};

static int written_kind(const char *name)
{
    for (int i = 0; i < (int)(sizeof written / sizeof *written); i++)
        if (ends_with(name, written[i].ext)) return i;
    return -1;
}

int fr_archive_writable(const char *name) { return written_kind(name) >= 0; }

char *fr_archive_stem(const char *name)
{
    size_t best = 0;
    for (size_t i = 0; i < sizeof browsable / sizeof *browsable; i++)
        if (ends_with(name, browsable[i]) && strlen(browsable[i]) > best) best = strlen(browsable[i]);
    size_t n = strlen(name);
    return fr_strndup(name, best ? n - best : fr_stem_len(name, 0));
}

static int set_written_kind(struct archive *w, int kind)
{
    if (kind == 0) return archive_write_set_format_zip(w) || archive_write_set_bytes_in_last_block(w, 1);
    if (kind == 1) return archive_write_set_format_7zip(w) || archive_write_set_bytes_in_last_block(w, 1);
    if (archive_write_set_format_pax_restricted(w)) return ARCHIVE_FATAL;
    return written[kind].filter ? written[kind].filter(w) : ARCHIVE_OK;
}

/* "./a//b/" -> "a/b"; NULL for a name reaching outside ("..") or none */
static char *entry_path(const char *name)
{
    if (!name) return NULL;
    char *out = fr_realloc(NULL, strlen(name) + 1), *o = out;
    const char *p = name;
    while (*p) {
        while (*p == '/') p++;
        const char *e = strchr(p, '/');
        if (!e) e = p + strlen(p);
        size_t n = (size_t)(e - p);
        if (!n) break;
        if (n == 2 && p[0] == '.' && p[1] == '.') return free(out), NULL;
        if (!(n == 1 && p[0] == '.')) {
            if (o != out) *o++ = '/';
            memcpy(o, p, n);
            o += n;
        }
        p = e;
    }
    *o = 0;
    if (!*out) return free(out), NULL;
    return out;
}

static const char *entry_name(struct archive_entry *e)
{
    const char *s = archive_entry_pathname_utf8(e);
    return s ? s : archive_entry_pathname(e);
}

/* path is base or below it ("" holds everything) */
static int path_within(const char *base, const char *path)
{
    size_t n = strlen(base);
    return !n || (!strncmp(base, path, n) && (path[n] == 0 || path[n] == '/'));
}

/* what is under base, as a path relative to it: "" for base itself */
static const char *path_rest(const char *base, const char *path)
{
    size_t n = strlen(base);
    if (!n) return path;
    return path[n] ? path + n + 1 : path + n;
}

/* Without iconv libarchive converts names only between UTF-8 and the locale's charset when that is UTF-8 too; the
 * program keeps the C locale, so the calls run under a UTF-8 one for this thread alone. */
static locale_t utf8_locale;
static pthread_once_t utf8_once = PTHREAD_ONCE_INIT;

static void utf8_make(void)
{
    utf8_locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
    if (!utf8_locale) utf8_locale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", (locale_t)0);
}

static locale_t utf8_enter(void)
{
    pthread_once(&utf8_once, utf8_make);
    return utf8_locale ? uselocale(utf8_locale) : (locale_t)0;
}

static void utf8_leave(locale_t previous)
{
    if (previous) uselocale(previous);
}

static int la_error(struct archive *a)
{
    int e = archive_errno(a);
    /* libarchive's own codes for a bad format (EILSEQ where there is no EFTYPE) and misuse (EINVAL) */
    return e > 0 && e != EILSEQ && e != EINVAL ? -e : -EBADMSG;
}

/* ---- reading the outer file ---- */

typedef struct {
    fr_stream *s;
    char buf[64 * 1024];
} read_ctx;

static la_ssize_t read_cb(struct archive *a, void *c, const void **buf)
{
    read_ctx *r = c;
    long n = r->s->read(r->s, r->buf, sizeof r->buf);
    if (n < 0) {
        archive_set_error(a, (int)-n, "%s", fr_vfs_error((int)n));
        return -1;
    }
    *buf = r->buf;
    return n;
}

static int read_close_cb(struct archive *a, void *c)
{
    read_ctx *r = c;
    r->s->close(r->s);
    free(r);
    return ARCHIVE_OK;
}

/* A file's own path lets libarchive seek, which 7z and a zip's central directory need; anywhere else the outer file
 * is read as a stream through its backend. */
static struct archive *reader_open(const char *outer, int *err)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    const char *local = fr_uri_local(outer);
    int r;
    if (local) r = archive_read_open_filename(a, local, 64 * 1024);
    else {
        fr_stream *s = fr_vfs_open_read(outer, err);
        if (!s) return archive_read_free(a), NULL;
        read_ctx *c = fr_realloc(NULL, sizeof *c);
        c->s = s;
        r = archive_read_open(a, c, NULL, read_cb, read_close_cb);
    }
    if (r != ARCHIVE_OK) {
        *err = la_error(a);
        archive_read_free(a);
        return NULL;
    }
    return a;
}

static int next_header(struct archive *a, struct archive_entry **e, int *err)
{
    int r = archive_read_next_header(a, e);
    if (r == ARCHIVE_OK || r == ARCHIVE_WARN) return 1;
    *err = r == ARCHIVE_EOF ? 0 : la_error(a);
    return 0;
}

/* ---- the tree of an archive's entries, read once and kept while the outer file stays the same ---- */

typedef struct {
    char *path; /* "" for the root */
    const char *name;
    int parent, child, sibling; /* -1 none */
    int is_dir, is_link;
    int index; /* the entry's place in the archive (its data's, for a hard link); -1 for a folder only implied */
    uint64_t size;
    int64_t mtime;
    uint32_t mode;
} node;

typedef struct {
    char *outer;
    int64_t mtime;
    uint64_t size;
    struct {
        node *v;
        int n, cap;
    } nodes;
    int *slots; /* node index + 1, 0 empty */
    size_t nslots;
    int refs, orphan;
} tree;

static size_t hash(const char *s)
{
    size_t h = 1469598103934665603u;
    while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211u;
    return h;
}

static int tree_find(const tree *t, const char *path)
{
    for (size_t i = hash(path) & (t->nslots - 1);; i = (i + 1) & (t->nslots - 1)) {
        int k = t->slots[i];
        if (!k) return -1;
        if (!strcmp(t->nodes.v[k - 1].path, path)) return k - 1;
    }
}

static void slot_put(tree *t, int idx)
{
    size_t i = hash(t->nodes.v[idx].path) & (t->nslots - 1);
    while (t->slots[i]) i = (i + 1) & (t->nslots - 1);
    t->slots[i] = idx + 1;
}

static int node_add(tree *t, char *path, int parent)
{
    if ((size_t)(t->nodes.n + 1) * 2 > t->nslots) {
        free(t->slots);
        t->nslots = t->nslots ? t->nslots * 2 : 64;
        t->slots = calloc(t->nslots, sizeof *t->slots);
        if (!t->slots) abort();
        for (int i = 0; i < t->nodes.n; i++) slot_put(t, i);
    }
    const char *slash = strrchr(path, '/');
    node nd = {.path = path, .name = slash ? slash + 1 : path, .parent = parent, .child = -1, .sibling = -1,
               .is_dir = 1, .index = -1, .mtime = t->mtime, .mode = 0755};
    FR_PUSH(t->nodes, nd);
    int idx = t->nodes.n - 1;
    slot_put(t, idx);
    if (parent >= 0) {
        t->nodes.v[idx].sibling = t->nodes.v[parent].child;
        t->nodes.v[parent].child = idx;
        t->nodes.v[parent].is_dir = 1;
    }
    return idx;
}

/* the node of path, made (as an implied folder, with its parents) when missing */
static int tree_ensure(tree *t, const char *path)
{
    int i = tree_find(t, path);
    if (i >= 0) return i;
    const char *slash = strrchr(path, '/');
    int parent = 0;
    if (slash) {
        char *up = fr_strndup(path, (size_t)(slash - path));
        parent = tree_ensure(t, up);
        free(up);
    }
    return node_add(t, fr_strdup(path), parent);
}

static void tree_add(tree *t, const char *path, struct archive_entry *e, int index)
{
    int i = tree_ensure(t, path);
    node *nd = &t->nodes.v[i];
    mode_t type = archive_entry_filetype(e);
    const char *hard = archive_entry_hardlink_utf8(e);
    if (!hard) hard = archive_entry_hardlink(e);
    char *target = hard ? entry_path(hard) : NULL;
    int from = target ? tree_find(t, target) : -1;
    free(target);
    nd->is_dir = type == AE_IFDIR || nd->child >= 0;
    nd->is_link = type == AE_IFLNK;
    nd->index = index;
    nd->size = type == AE_IFREG && archive_entry_size_is_set(e) ? (uint64_t)archive_entry_size(e) : 0;
    if (from >= 0 && !t->nodes.v[from].is_dir) nd->index = t->nodes.v[from].index, nd->size = t->nodes.v[from].size;
    if (nd->is_dir) nd->size = 0;
    if (archive_entry_mtime_is_set(e)) nd->mtime = (int64_t)archive_entry_mtime(e);
    nd->mode = (uint32_t)archive_entry_perm(e) & 07777;
    if (!nd->mode) nd->mode = nd->is_dir ? 0755 : 0644;
}

static void tree_free(tree *t)
{
    for (int i = 0; i < t->nodes.n; i++) free(t->nodes.v[i].path);
    free(t->nodes.v);
    free(t->slots);
    free(t->outer);
    free(t);
}

static tree *tree_read(const char *outer, const fr_entry *st, int *err)
{
    struct archive *a = reader_open(outer, err);
    if (!a) return NULL;
    tree *t = fr_realloc(NULL, sizeof *t);
    *t = (tree){.outer = fr_strdup(outer), .mtime = st->mtime, .size = st->size};
    node_add(t, fr_strdup(""), -1);
    struct archive_entry *e;
    int index = 0;
    *err = 0;
    while (next_header(a, &e, err)) {
        char *p = entry_path(entry_name(e));
        if (p) tree_add(t, p, e, index);
        free(p);
        index++;
    }
    archive_read_free(a);
    if (*err) return tree_free(t), NULL;
    return t;
}

static tree *tree_build(const char *outer, const fr_entry *st, int *err)
{
    locale_t previous = utf8_enter();
    tree *t = tree_read(outer, st, err);
    utf8_leave(previous);
    return t;
}

#define CACHE_MAX 8
static tree *cache[CACHE_MAX]; /* the most recently used first */
static pthread_mutex_t cache_lock;
static pthread_once_t cache_once = PTHREAD_ONCE_INIT;

/* an archive in an archive reads its outer file through this backend again, under the same lock */
static void cache_init(void)
{
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cache_lock, &at);
    pthread_mutexattr_destroy(&at);
}

static void cache_lock_take(void)
{
    pthread_once(&cache_once, cache_init);
    pthread_mutex_lock(&cache_lock);
}

static void cache_drop_locked(int i)
{
    tree *t = cache[i];
    memmove(cache + i, cache + i + 1, sizeof *cache * (size_t)(CACHE_MAX - i - 1));
    cache[CACHE_MAX - 1] = NULL;
    if (t->refs) t->orphan = 1;
    else tree_free(t);
}

static tree *tree_get(const char *outer, int *err)
{
    fr_entry st = {0};
    *err = fr_vfs_stat(outer, &st);
    int is_dir = st.is_dir;
    fr_entry_free(&st);
    if (*err) return NULL;
    if (is_dir) return *err = -ENOTDIR, NULL;
    cache_lock_take();
    tree *t = NULL;
    for (int i = 0; i < CACHE_MAX && cache[i] && !t; i++) {
        if (strcmp(cache[i]->outer, outer)) continue;
        if (cache[i]->mtime == st.mtime && cache[i]->size == st.size) {
            t = cache[i];
            memmove(cache + 1, cache, sizeof *cache * (size_t)i);
            cache[0] = t;
        } else cache_drop_locked(i);
    }
    if (!t && (t = tree_build(outer, &st, err))) {
        if (cache[CACHE_MAX - 1]) cache_drop_locked(CACHE_MAX - 1);
        memmove(cache + 1, cache, sizeof *cache * (CACHE_MAX - 1));
        cache[0] = t;
    }
    if (t) t->refs++;
    pthread_mutex_unlock(&cache_lock);
    return t;
}

static void tree_put(tree *t)
{
    cache_lock_take();
    if (!--t->refs && t->orphan) tree_free(t);
    pthread_mutex_unlock(&cache_lock);
}

static void cache_forget(const char *outer)
{
    cache_lock_take();
    for (int i = CACHE_MAX - 1; i >= 0; i--)
        if (cache[i] && !strcmp(cache[i]->outer, outer)) cache_drop_locked(i);
    pthread_mutex_unlock(&cache_lock);
}

void fr_archive_cache_clear(void)
{
    cache_lock_take();
    while (cache[0]) cache_drop_locked(0);
    pthread_mutex_unlock(&cache_lock);
}

/* ---- the backend ---- */

typedef struct {
    char *outer, *inner;
    tree *t;
    int node; /* -1 when not there */
} spot;

static int spot_open(const char *uri, spot *s)
{
    *s = (spot){.node = -1};
    s->outer = fr_archive_outer(uri);
    if (!s->outer) return -EINVAL;
    s->inner = fr_archive_inner(uri);
    int err = 0;
    s->t = tree_get(s->outer, &err);
    if (!s->t) return err;
    s->node = tree_find(s->t, s->inner);
    return s->node < 0 ? -ENOENT : 0;
}

static void spot_close(spot *s)
{
    if (s->t) tree_put(s->t);
    free(s->outer);
    free(s->inner);
}

static void fill(const spot *s, int i, fr_entry *e)
{
    const node *nd = &s->t->nodes.v[i];
    if (*nd->path) e->name = fr_strdup(nd->name);
    else {
        e->name = fr_uri_name(s->outer);
        if (!*e->name) free(e->name), e->name = fr_strdup("/");
    }
    e->is_dir = nd->is_dir;
    e->is_link = nd->is_link;
    e->hidden = e->name[0] == '.';
    e->size = nd->size;
    e->mtime = nd->mtime;
    e->mode = nd->mode;
}

static int ar_list(const char *uri, fr_entries *out)
{
    spot s;
    int rc = spot_open(uri, &s);
    if (!rc && !s.t->nodes.v[s.node].is_dir) rc = -ENOTDIR;
    for (int c = rc ? -1 : s.t->nodes.v[s.node].child; c >= 0; c = s.t->nodes.v[c].sibling) {
        fr_entry e = {0};
        fill(&s, c, &e);
        FR_PUSH(*out, e);
    }
    spot_close(&s);
    return rc;
}

static int ar_stat(const char *uri, fr_entry *out)
{
    spot s;
    int rc = spot_open(uri, &s);
    if (!rc) fill(&s, s.node, out);
    spot_close(&s);
    return rc;
}

typedef struct {
    fr_stream s;
    struct archive *a;
} entry_stream;

static long entry_read(fr_stream *s, void *buf, size_t n)
{
    struct archive *a = ((entry_stream *)s)->a;
    la_ssize_t r = archive_read_data(a, buf, n);
    return r < 0 ? la_error(a) : (long)r;
}

static long entry_write(fr_stream *s, const void *buf, size_t n) { return -EBADF; }

static int entry_close(fr_stream *s)
{
    archive_read_free(((entry_stream *)s)->a);
    free(s);
    return 0;
}

/* a reader standing at the entry with that index */
static struct archive *reader_at(const char *outer, int index, struct archive_entry **e, int *err)
{
    struct archive *a = reader_open(outer, err);
    for (int i = 0; a; i++) {
        if (!next_header(a, e, err)) {
            if (!*err) *err = -ENOENT;
            archive_read_free(a);
            return NULL;
        }
        if (i == index) break;
    }
    return a;
}

static fr_stream *ar_open_read(const char *uri, int *err)
{
    spot s;
    *err = spot_open(uri, &s);
    struct archive *a = NULL;
    if (!*err && (s.t->nodes.v[s.node].is_dir || s.t->nodes.v[s.node].index < 0)) *err = -EISDIR;
    if (!*err) {
        struct archive_entry *e;
        locale_t previous = utf8_enter();
        a = reader_at(s.outer, s.t->nodes.v[s.node].index, &e, err);
        utf8_leave(previous);
        if (a && archive_entry_is_encrypted(e)) *err = -EACCES, archive_read_free(a), a = NULL;
    }
    spot_close(&s);
    if (!a) return NULL;
    entry_stream *es = fr_realloc(NULL, sizeof *es);
    *es = (entry_stream){{entry_read, entry_write, entry_close}, a};
    return &es->s;
}

/* the checks every change inside an archive makes: a kind ferret writes, not in another archive */
static int writable_outer(const char *outer)
{
    if (fr_archive_is_uri(outer)) return -ENOTSUP;
    char *name = fr_uri_name(outer);
    int ok = fr_archive_writable(name);
    free(name);
    return ok ? 0 : -ENOTSUP;
}

/* where path would go: its parent must be a folder there */
static int parent_ok(const spot *s, const char *path)
{
    const char *slash = strrchr(path, '/');
    char *up = slash ? fr_strndup(path, (size_t)(slash - path)) : fr_strdup("");
    int p = tree_find(s->t, up);
    free(up);
    return p < 0 ? -ENOENT : s->t->nodes.v[p].is_dir ? 0 : -ENOTDIR;
}

static int one_change(const char *uri, fr_archive_change c, int want_there)
{
    spot s;
    int rc = spot_open(uri, &s);
    if (want_there && rc) return spot_close(&s), rc;
    if (!want_there) {
        if (!rc) return spot_close(&s), -EEXIST;
        if (rc != -ENOENT || !s.t) return spot_close(&s), rc;
        if ((rc = parent_ok(&s, s.inner))) return spot_close(&s), rc;
    }
    if (!(rc = writable_outer(s.outer))) {
        c.path = s.inner;
        rc = fr_archive_rewrite(s.outer, &c, 1, NULL);
    }
    spot_close(&s);
    return rc;
}

static int ar_mkdir(const char *uri) { return one_change(uri, (fr_archive_change){.op = FR_AR_MKDIR}, 0); }

static int ar_remove(const char *uri)
{
    spot s;
    int rc = spot_open(uri, &s);
    if (!rc && !*s.inner) rc = -EBUSY;
    if (!rc && s.t->nodes.v[s.node].child >= 0) rc = -ENOTEMPTY;
    spot_close(&s);
    return rc ? rc : one_change(uri, (fr_archive_change){.op = FR_AR_DELETE}, 1);
}

static int ar_rename(const char *from, const char *to)
{
    spot s, d;
    int rc = spot_open(from, &s), there = spot_open(to, &d);
    if (!rc && !*s.inner) rc = -EBUSY;
    if (!rc && !there) rc = strcmp(s.inner, d.inner) ? -EEXIST : 0;
    else if (!rc && there != -ENOENT) rc = there;
    else if (!rc) rc = parent_ok(&d, d.inner);
    if (!rc && path_within(s.inner, d.inner) && strcmp(s.inner, d.inner)) rc = -EINVAL;
    if (!rc && strcmp(s.inner, d.inner) && !(rc = writable_outer(s.outer))) {
        fr_archive_change c = {.op = FR_AR_RENAME, .path = s.inner, .to = d.inner};
        rc = fr_archive_rewrite(s.outer, &c, 1, NULL);
    }
    spot_close(&s);
    spot_close(&d);
    return rc;
}

static unsigned temp_serial;

/* a hidden name beside outer for what is being written */
static char *temp_beside(const char *outer)
{
    char *dir = fr_uri_parent(outer), *name = fr_uri_name(outer);
    char *tmp = fr_format(".%s.ferret-%ld-%u", name, (long)getpid(), __atomic_add_fetch(&temp_serial, 1, __ATOMIC_RELAXED));
    char *uri = dir ? fr_uri_join(dir, tmp) : NULL;
    free(dir), free(name), free(tmp);
    return uri;
}

/* writing one file: it is spooled beside the archive, then added when closed */
typedef struct {
    fr_stream s;
    fr_stream *spool;
    char *outer, *inner, *tmp;
    int replace;
} add_stream;

static long add_write(fr_stream *s, const void *buf, size_t n)
{
    add_stream *a = (add_stream *)s;
    return a->spool->write(a->spool, buf, n);
}

static long add_read(fr_stream *s, void *buf, size_t n) { return -EBADF; }

static int add_close(fr_stream *s)
{
    add_stream *a = (add_stream *)s;
    int rc = a->spool->close(a->spool);
    if (!rc) {
        fr_archive_change c[2] = {{.op = FR_AR_DELETE, .path = a->inner}, {.op = FR_AR_ADD, .path = a->inner, .src = a->tmp}};
        rc = fr_archive_rewrite(a->outer, c + !a->replace, 2 - !a->replace, NULL);
    }
    fr_vfs_remove(a->tmp);
    free(a->outer), free(a->inner), free(a->tmp);
    free(a);
    return rc;
}

static fr_stream *ar_open_write(const char *uri, int exclusive, int *err)
{
    spot s;
    int rc = spot_open(uri, &s), replace = 0;
    if (!rc) {
        if (exclusive) rc = -EEXIST;
        else if (s.t->nodes.v[s.node].is_dir) rc = -EISDIR;
        else replace = 1;
    } else if (rc == -ENOENT && s.t) rc = parent_ok(&s, s.inner);
    if (!rc) rc = writable_outer(s.outer);
    char *tmp = rc ? NULL : temp_beside(s.outer);
    if (!rc && !tmp) rc = -ENOTSUP;
    fr_stream *spool = rc ? NULL : fr_vfs_open_write(tmp, 1, &rc);
    if (!spool) {
        free(tmp);
        spot_close(&s);
        return *err = rc, NULL;
    }
    add_stream *a = fr_realloc(NULL, sizeof *a);
    *a = (add_stream){{add_read, add_write, add_close}, spool, s.outer, s.inner, tmp, replace};
    s.outer = s.inner = NULL;
    spot_close(&s);
    return &a->s;
}

const fr_backend fr_backend_archive = {
    .scheme = "archive",
    .list = ar_list,
    .stat = ar_stat,
    .open_read = ar_open_read,
    .open_write = ar_open_write,
    .mkdir = ar_mkdir,
    .rename = ar_rename,
    .remove = ar_remove,
};

/* ---- hooks ---- */

static int cancelled(const fr_archive_hooks *h) { return h && h->cancel && __atomic_load_n(h->cancel, __ATOMIC_RELAXED); }

static void h_total(const fr_archive_hooks *h, uint64_t bytes, int files)
{
    if (h && h->total) h->total(h->ctx, bytes, files);
}

static void h_progress(const fr_archive_hooks *h, uint64_t bytes, int files)
{
    if (h && h->progress) h->progress(h->ctx, bytes, files);
}

static void h_current(const fr_archive_hooks *h, const char *name)
{
    if (h && h->current) h->current(h->ctx, name);
}

static void h_fail(const fr_archive_hooks *h, const char *uri, int err)
{
    if (h && h->fail) h->fail(h->ctx, uri, err);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ---- writing an archive ---- */

typedef struct {
    struct archive *w;
    const fr_archive_hooks *h;
    const char *outer, *tmp; /* never added into the archive */
    char *buf;
} writer;

typedef struct {
    fr_stream *s;
} write_ctx;

static la_ssize_t write_cb(struct archive *a, void *c, const void *buf, size_t n)
{
    write_ctx *w = c;
    long r = w->s->write(w->s, buf, n);
    if (r < 0) {
        archive_set_error(a, (int)-r, "%s", fr_vfs_error((int)r));
        return -1;
    }
    return r;
}

static int write_close_cb(struct archive *a, void *c)
{
    write_ctx *w = c;
    int rc = w->s ? w->s->close(w->s) : 0;
    w->s = NULL;
    if (rc) {
        archive_set_error(a, -rc, "%s", fr_vfs_error(rc));
        return ARCHIVE_FATAL;
    }
    return ARCHIVE_OK;
}

static int write_free_cb(struct archive *a, void *c)
{
    write_ctx *w = c;
    if (w->s) w->s->close(w->s);
    free(w);
    return ARCHIVE_OK;
}

/* the bytes and items under each source, for the job's progress */
static void count_sources(const fr_archive_change *ch, int n, const fr_archive_hooks *h)
{
    for (int i = 0; i < n && h && h->total; i++) {
        if (ch[i].op == FR_AR_MKDIR || (ch[i].op == FR_AR_ADD && !ch[i].src)) h_total(h, 0, 1);
        if (ch[i].op != FR_AR_ADD || !ch[i].src) continue;
        fr_tree_count c = {0};
        fr_vfs_measure(ch[i].src, &c, h->cancel ? h->cancel : &(int){0});
        h_total(h, c.bytes, (int)(c.files + c.folders));
    }
}

static int copy_data(writer *wr, fr_stream *in, uint64_t size)
{
    uint64_t done = 0;
    for (;;) {
        if (cancelled(wr->h)) return -ECANCELED;
        long n = in->read(in, wr->buf, BUF_SIZE);
        if (n < 0) return (int)n;
        if (!n) return 0;
        size_t take = done + (uint64_t)n > size ? (size_t)(size - done) : (size_t)n;
        if (take && archive_write_data(wr->w, wr->buf, take) < 0) return la_error(wr->w);
        done += take;
        h_progress(wr->h, (uint64_t)n, 0);
    }
}

static int add_tree(writer *wr, const char *src, const char *path)
{
    if (cancelled(wr->h)) return -ECANCELED;
    if (!strcmp(src, wr->outer) || !strcmp(src, wr->tmp)) return 0;
    fr_entry e = {0};
    int rc = fr_vfs_stat(src, &e);
    if (rc) return h_fail(wr->h, src, rc), rc;
    h_current(wr->h, e.name);
    struct archive_entry *ae = archive_entry_new();
    archive_entry_set_pathname_utf8(ae, path);
    archive_entry_set_mtime(ae, (time_t)e.mtime, 0);
    archive_entry_set_perm(ae, e.mode ? e.mode : e.is_dir ? 0755 : 0644);
    const char *local = fr_uri_local(src);
    char target[4096];
    ssize_t tn = e.is_link && local ? readlink(local, target, sizeof target - 1) : -1;
    if (tn >= 0) {
        target[tn] = 0;
        archive_entry_set_filetype(ae, AE_IFLNK);
        archive_entry_set_symlink_utf8(ae, target);
        if (archive_write_header(wr->w, ae) < ARCHIVE_WARN) rc = la_error(wr->w);
    } else if (e.is_dir) {
        archive_entry_set_filetype(ae, AE_IFDIR);
        if (archive_write_header(wr->w, ae) < ARCHIVE_WARN) rc = la_error(wr->w);
        fr_entries kids = {0};
        if (!rc && (rc = fr_vfs_list(src, &kids))) h_fail(wr->h, src, rc);
        for (int i = 0; !rc && i < kids.n; i++) {
            char *from = fr_uri_join(src, kids.v[i].name), *to = fr_format("%s/%s", path, kids.v[i].name);
            rc = add_tree(wr, from, to);
            free(from), free(to);
        }
        fr_entries_free(&kids);
        archive_entry_free(ae);
        fr_entry_free(&e);
        if (!rc) h_progress(wr->h, 0, 1);
        return rc;
    } else {
        archive_entry_set_filetype(ae, AE_IFREG);
        archive_entry_set_size(ae, (la_int64_t)e.size);
        fr_stream *in = fr_vfs_open_read(src, &rc);
        if (in) {
            if (archive_write_header(wr->w, ae) < ARCHIVE_WARN) rc = la_error(wr->w);
            else rc = copy_data(wr, in, e.size);
            in->close(in);
        }
    }
    if (rc && rc != -ECANCELED) h_fail(wr->h, src, rc);
    else if (!rc) h_progress(wr->h, 0, 1);
    archive_entry_free(ae);
    fr_entry_free(&e);
    return rc;
}

/* the path an old entry keeps after the changes: NULL when it goes */
static char *after_changes(const char *path, const fr_archive_change *ch, int n)
{
    char *p = fr_strdup(path);
    for (int i = 0; i < n && p; i++) {
        if (!path_within(ch[i].path, p)) continue;
        if (ch[i].op == FR_AR_DELETE) {
            free(p);
            p = NULL;
        } else if (ch[i].op == FR_AR_RENAME) {
            const char *rest = path_rest(ch[i].path, p);
            char *q = *rest ? fr_format("%s/%s", ch[i].to, rest) : fr_strdup(ch[i].to);
            free(p);
            p = q;
        }
    }
    return p;
}

static int copy_old(writer *wr, const char *outer, const fr_archive_change *ch, int n)
{
    int err = 0;
    struct archive *r = reader_open(outer, &err);
    if (!r) return err;
    struct archive_entry *e;
    while (!err && next_header(r, &e, &err)) {
        if (cancelled(wr->h)) {
            err = -ECANCELED;
            break;
        }
        char *p = entry_path(entry_name(e)), *to = p ? after_changes(p, ch, n) : NULL;
        if (p && !to) {
            free(p);
            continue;
        }
        struct archive_entry *copy = archive_entry_clone(e);
        if (to && strcmp(p, to)) archive_entry_set_pathname_utf8(copy, to);
        const char *hard = archive_entry_hardlink_utf8(e);
        char *hp = hard ? entry_path(hard) : NULL, *hto = hp ? after_changes(hp, ch, n) : NULL;
        if (hto && strcmp(hp, hto)) archive_entry_set_hardlink_utf8(copy, hto);
        free(hp), free(hto);
        h_current(wr->h, base_name(to ? to : entry_name(e)));
        if (archive_write_header(wr->w, copy) < ARCHIVE_WARN) err = la_error(wr->w);
        else if (archive_entry_filetype(e) == AE_IFREG && archive_entry_size(e) > 0) {
            for (;;) {
                la_ssize_t got = archive_read_data(r, wr->buf, BUF_SIZE);
                if (got < 0) err = la_error(r);
                if (got <= 0) break;
                if (archive_write_data(wr->w, wr->buf, (size_t)got) < 0) {
                    err = la_error(wr->w);
                    break;
                }
                h_progress(wr->h, (uint64_t)got, 0);
                if (cancelled(wr->h)) {
                    err = -ECANCELED;
                    break;
                }
            }
        }
        if (!err) h_progress(wr->h, 0, 1);
        archive_entry_free(copy);
        free(p), free(to);
    }
    archive_read_free(r);
    return err;
}

static int write_archive(const char *outer, int create, const fr_archive_change *ch, int n, const fr_archive_hooks *h)
{
    int rc = writable_outer(outer);
    if (rc) return rc;
    fr_entry old = {0};
    if (create) {
        if (fr_vfs_exists(outer)) return -EEXIST;
    } else if ((rc = fr_vfs_stat(outer, &old))) return rc;
    if (!create && h && h->total) {
        int err = 0;
        tree *t = tree_get(outer, &err);
        if (t) {
            uint64_t bytes = 0;
            for (int i = 0; i < t->nodes.n; i++) bytes += t->nodes.v[i].size;
            h_total(h, bytes, t->nodes.n - 1);
            tree_put(t);
        }
    }
    count_sources(ch, n, h);
    char *name = fr_uri_name(outer), *tmp = temp_beside(outer);
    int kind = written_kind(name);
    free(name);
    fr_stream *out = tmp ? fr_vfs_open_write(tmp, 1, &rc) : NULL;
    if (!out) {
        free(tmp);
        fr_entry_free(&old);
        return rc ? rc : -ENOTSUP;
    }
    writer wr = {archive_write_new(), h, outer, tmp, fr_realloc(NULL, BUF_SIZE)};
    write_ctx *wc = fr_realloc(NULL, sizeof *wc);
    wc->s = out;
    if (set_written_kind(wr.w, kind)) {
        rc = la_error(wr.w);
        write_free_cb(wr.w, wc);
    } else if (archive_write_open2(wr.w, wc, NULL, write_cb, write_close_cb, write_free_cb)) rc = la_error(wr.w);
    if (!rc && !create) rc = copy_old(&wr, outer, ch, n);
    for (int i = 0; !rc && i < n; i++) {
        if (ch[i].op == FR_AR_MKDIR || (ch[i].op == FR_AR_ADD && !ch[i].src)) {
            int dir = ch[i].op == FR_AR_MKDIR;
            struct archive_entry *ae = archive_entry_new();
            archive_entry_set_pathname_utf8(ae, ch[i].path);
            archive_entry_set_filetype(ae, dir ? AE_IFDIR : AE_IFREG);
            archive_entry_set_perm(ae, dir ? 0755 : 0644);
            if (!dir) archive_entry_set_size(ae, 0);
            archive_entry_set_mtime(ae, time(NULL), 0);
            if (archive_write_header(wr.w, ae) < ARCHIVE_WARN) rc = la_error(wr.w);
            else h_progress(h, 0, 1);
            archive_entry_free(ae);
        } else if (ch[i].op == FR_AR_ADD) rc = add_tree(&wr, ch[i].src, ch[i].path);
    }
    if (archive_write_close(wr.w) != ARCHIVE_OK && !rc) rc = la_error(wr.w);
    archive_write_free(wr.w);
    free(wr.buf);
    if (!rc && cancelled(h)) rc = -ECANCELED;
    if (!rc) {
        if (!create && old.mode) fr_vfs_set_mode(tmp, old.mode);
        const char *a = fr_uri_local(tmp), *b = fr_uri_local(outer);
        if (a && b) rc = rename(a, b) ? -errno : 0;
        else {
            if (!create) rc = fr_vfs_remove(outer);
            if (!rc) rc = fr_vfs_rename(tmp, outer);
        }
    }
    if (rc) fr_vfs_remove(tmp);
    cache_forget(outer);
    free(tmp);
    fr_entry_free(&old);
    return rc;
}

int fr_archive_rewrite(const char *outer, const fr_archive_change *changes, int n, const fr_archive_hooks *h)
{
    locale_t previous = utf8_enter();
    int rc = write_archive(outer, 0, changes, n, h);
    utf8_leave(previous);
    return rc;
}

int fr_archive_create(const char *outer, const fr_strs *sources, const fr_archive_hooks *h)
{
    fr_archive_change *ch = fr_realloc(NULL, sizeof *ch * (size_t)(sources->n ? sources->n : 1));
    char **names = fr_realloc(NULL, sizeof *names * (size_t)(sources->n ? sources->n : 1));
    for (int i = 0; i < sources->n; i++) {
        names[i] = fr_uri_name(sources->v[i]);
        ch[i] = (fr_archive_change){.op = FR_AR_ADD, .path = names[i], .src = sources->v[i]};
    }
    locale_t previous = utf8_enter();
    int rc = write_archive(outer, 1, ch, sources->n, h);
    utf8_leave(previous);
    for (int i = 0; i < sources->n; i++) free(names[i]);
    free(names), free(ch);
    return rc;
}

/* ---- extracting ---- */

/* the folders down to uri, below base (which is made too) */
static int make_dirs(const char *base, const char *rel)
{
    int rc = fr_vfs_mkdir(base);
    if (rc && rc != -EEXIST) return rc;
    for (const char *s = rel; *s; s++) {
        if (*s != '/') continue;
        char *part = fr_strndup(rel, (size_t)(s - rel)), *uri = fr_uri_join(base, part);
        rc = fr_vfs_mkdir(uri);
        free(part), free(uri);
        if (rc && rc != -EEXIST) return rc;
    }
    return 0;
}

static int write_file(writer *wr, const char *dest, fr_stream *in, struct archive *r)
{
    int rc = 0;
    fr_stream *out = fr_vfs_open_write(dest, 0, &rc);
    if (!out) return rc;
    for (;;) {
        if (cancelled(wr->h)) {
            rc = -ECANCELED;
            break;
        }
        long got = in ? in->read(in, wr->buf, BUF_SIZE) : (long)archive_read_data(r, wr->buf, BUF_SIZE);
        if (got < 0) rc = in ? (int)got : la_error(r);
        if (got <= 0) break;
        long put = out->write(out, wr->buf, (size_t)got);
        if (put < 0) {
            rc = (int)put;
            break;
        }
        h_progress(wr->h, (uint64_t)got, 0);
    }
    int closed = out->close(out);
    if (!rc) rc = closed;
    if (rc) fr_vfs_remove(dest);
    return rc;
}

typedef struct {
    char *dest, *target;
} pending_link;

typedef struct {
    pending_link *v;
    int n, cap;
} pending_links;

/* the entry the reader stands at, to dest; symlinks wait in links until the files are out, so none of them is
 * written through a link the archive made */
static int extract_one(writer *wr, struct archive *r, struct archive_entry *e, const char *dest, char **base,
                       const char *const *to, int n, pending_links *links)
{
    mode_t type = archive_entry_filetype(e);
    const char *hard = archive_entry_hardlink_utf8(e);
    if (!hard) hard = archive_entry_hardlink(e);
    int rc = 0;
    if (type == AE_IFLNK) {
        const char *target = archive_entry_symlink_utf8(e);
        if (!target) target = archive_entry_symlink(e);
        if (target) FR_PUSH(*links, ((pending_link){fr_strdup(dest), fr_strdup(target)}));
        return 0;
    }
    if (type == AE_IFDIR) {
        rc = fr_vfs_mkdir(dest);
        if (rc == -EEXIST) rc = 0;
    } else if (archive_entry_is_encrypted(e)) rc = -EACCES;
    else if (hard) {
        char *hp = entry_path(hard), *src = NULL;
        for (int k = 0; hp && k < n && !src; k++)
            if (path_within(base[k], hp)) {
                const char *rest = path_rest(base[k], hp);
                src = *rest ? fr_uri_join(to[k], rest) : fr_strdup(to[k]);
            }
        fr_stream *in = src ? fr_vfs_open_read(src, &rc) : NULL;
        if (!src) rc = -ENOENT;
        if (in) {
            rc = write_file(wr, dest, in, NULL);
            in->close(in);
        }
        free(hp), free(src);
    } else if (type == AE_IFREG) rc = write_file(wr, dest, NULL, r);
    else return 0;
    if (rc) return rc;
    const fr_backend *b = fr_vfs_backend(dest);
    if (b->set_mtime && archive_entry_mtime_is_set(e)) b->set_mtime(dest, (int64_t)archive_entry_mtime(e));
    uint32_t perm = (uint32_t)archive_entry_perm(e) & 0777;
    if (b->set_mode && perm && type == AE_IFREG) b->set_mode(dest, perm | 0600);
    h_progress(wr->h, 0, 1);
    return 0;
}

static int extract(const char *outer, const char *const *from, const char *const *to, int n, int *failed,
                   const fr_archive_hooks *h)
{
    int err = 0;
    tree *t = tree_get(outer, &err);
    if (!t) return err;
    char **base = fr_realloc(NULL, sizeof *base * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) {
        base[i] = clean_path(from[i]);
        if (failed) failed[i] = 0;
        int k = tree_find(t, base[i]);
        if (k < 0) {
            h_fail(h, to[i], -ENOENT);
            if (failed) failed[i] = 1;
            continue;
        }
        uint64_t bytes = 0;
        int files = 0;
        for (int j = 0; j < t->nodes.n; j++)
            if (*t->nodes.v[j].path && path_within(base[i], t->nodes.v[j].path)) bytes += t->nodes.v[j].size, files++;
        h_total(h, bytes, files);
        if (t->nodes.v[k].is_dir && (err = make_dirs(to[i], ""))) {
            h_fail(h, to[i], err);
            if (failed) failed[i] = 1;
        }
    }
    tree_put(t);
    err = 0;
    struct archive *r = reader_open(outer, &err);
    writer wr = {NULL, h, NULL, NULL, fr_realloc(NULL, BUF_SIZE)};
    pending_links links = {0};
    struct archive_entry *e;
    int first = 0;
    while (r && next_header(r, &e, &err)) {
        if (cancelled(h)) {
            err = -ECANCELED;
            break;
        }
        char *p = entry_path(entry_name(e));
        int i = 0;
        while (p && i < n && !path_within(base[i], p)) i++;
        if (!p || i == n) {
            free(p);
            continue;
        }
        const char *rest = path_rest(base[i], p);
        char *dest = *rest ? fr_uri_join(to[i], rest) : fr_strdup(to[i]);
        h_current(h, base_name(p));
        int rc = *rest ? make_dirs(to[i], rest) : 0;
        if (!rc) rc = extract_one(&wr, r, e, dest, base, to, n, &links);
        if (rc == -ECANCELED) err = rc;
        else if (rc) {
            h_fail(h, dest, rc);
            if (failed) failed[i] = 1;
            if (!first) first = rc;
        }
        free(dest), free(p);
    }
    if (r) archive_read_free(r);
    for (int k = 0; k < links.n; k++) {
        const char *path = fr_uri_local(links.v[k].dest);
        if (!err && path) {
            unlink(path);
            if (symlink(links.v[k].target, path)) {
                h_fail(h, links.v[k].dest, -errno);
                if (!first) first = -errno;
            } else h_progress(h, 0, 1);
        }
        free(links.v[k].dest), free(links.v[k].target);
    }
    free(links.v);
    free(wr.buf);
    for (int i = 0; i < n; i++) free(base[i]);
    free(base);
    return err ? err : first;
}

int fr_archive_extract(const char *outer, const char *const *from, const char *const *to, int n, int *failed,
                       const fr_archive_hooks *h)
{
    locale_t previous = utf8_enter();
    int rc = extract(outer, from, to, n, failed, h);
    utf8_leave(previous);
    return rc;
}

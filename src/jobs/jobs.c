#include "jobs.h"
#include "core/opencache.h"
#include "vfs/vfs.h"
#include "vfs/vfs_archive.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    fr_job_status st;
    fr_strs sources;
    char *dest;
    int cancel;
    fr_conflict policy; /* an answer given "for all" */
    fr_conflict answer;
    int answered;
    uint32_t mode_set, mode_clear;
    int flag; /* JOB_EXTRACT: each into a folder of its own; JOB_CREATE: a folder */
    fr_steps todo; /* JOB_STEPS */
    /* the worker's alone until the job is done; then the window's, through fr_job_take_log */
    fr_steps log;
} job;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cond = PTHREAD_COND_INITIALIZER, idle_cond = PTHREAD_COND_INITIALIZER;
static struct {
    job **v;
    int n, cap;
} jobs;
static pthread_t worker;
static int started, stopping, next_id = 1, finished;
static void (*wake_fn)(void);
static double last_wake;

static void wake_locked(int force)
{
    double now = fr_now();
    if (wake_fn && (force || now - last_wake > 1.0 / 15)) {
        last_wake = now;
        wake_fn();
    }
}

static int cancelled(job *j) { return __atomic_load_n(&j->cancel, __ATOMIC_RELAXED); }

static void set_current(job *j, const char *uri)
{
    char *name = fr_uri_name(uri);
    pthread_mutex_lock(&lock);
    snprintf(j->st.current, sizeof j->st.current, "%s", name);
    wake_locked(0);
    pthread_mutex_unlock(&lock);
    free(name);
}

static void fail(job *j, const char *uri, int err, const char *why)
{
    char *name = fr_uri_name(uri);
    pthread_mutex_lock(&lock);
    j->st.failed++;
    if (!j->st.error[0]) snprintf(j->st.error, sizeof j->st.error, "%s: %s", name, why ? why : fr_vfs_error(err));
    wake_locked(1);
    pthread_mutex_unlock(&lock);
    free(name);
}

static void add_progress(job *j, uint64_t bytes, int files)
{
    pthread_mutex_lock(&lock);
    j->st.bytes_done += bytes;
    j->st.files_done += files;
    wake_locked(0);
    pthread_mutex_unlock(&lock);
}

/* ---- walking trees ---- */

static void scan(job *j, const char *uri, const fr_entry *e, uint64_t *bytes, int *files)
{
    (*files)++;
    if (e->is_link || !e->is_dir) {
        *bytes += e->size;
        return;
    }
    fr_entries kids = {0};
    if (!fr_vfs_list(uri, &kids))
        for (int i = 0; i < kids.n && !cancelled(j); i++) {
            char *child = fr_uri_join(uri, kids.v[i].name);
            scan(j, child, &kids.v[i], bytes, files);
            free(child);
        }
    fr_entries_free(&kids);
}

/* deletes uri and everything under it; 0 or the first error */
static int remove_tree(job *j, const char *uri, int count)
{
    fr_entry e = {0};
    int rc = fr_vfs_stat(uri, &e);
    if (rc) return rc;
    if (e.is_dir && !e.is_link) {
        fr_entries kids = {0};
        rc = fr_vfs_list(uri, &kids);
        for (int i = 0; !rc && i < kids.n; i++) {
            if (j && cancelled(j)) rc = -ECANCELED;
            else {
                char *child = fr_uri_join(uri, kids.v[i].name);
                rc = remove_tree(j, child, count);
                free(child);
            }
        }
        fr_entries_free(&kids);
    }
    if (!rc) rc = fr_vfs_remove(uri);
    if (!rc && j && count) add_progress(j, 0, 1);
    fr_entry_free(&e);
    return rc;
}

static int copy_file(job *j, const char *src, const fr_entry *e, const char *dst, int replace)
{
    int err = 0;
    fr_stream *in = fr_vfs_open_read(src, &err);
    if (!in) return err;
    fr_stream *out = fr_vfs_open_write(dst, !replace, &err);
    if (!out) {
        in->close(in);
        return err;
    }
    static char buf[256 * 1024];
    int rc = 0;
    for (;;) {
        if (cancelled(j)) {
            rc = -ECANCELED;
            break;
        }
        long n = in->read(in, buf, sizeof buf);
        if (n < 0) rc = (int)n;
        if (n <= 0) break;
        long w = out->write(out, buf, (size_t)n);
        if (w < 0) {
            rc = (int)w;
            break;
        }
        add_progress(j, (uint64_t)n, 0);
    }
    in->close(in);
    int closed = out->close(out);
    if (!rc) rc = closed;
    if (rc) fr_vfs_remove(dst);
    else {
        const fr_backend *b = fr_vfs_backend(dst);
        if (b->set_mtime) b->set_mtime(dst, e->mtime);
    }
    return rc;
}

/* copies src (described by e) to dst; replace: what is at dst gives way, folders merge */
static int copy_tree(job *j, const char *src, const fr_entry *e, const char *dst, int replace)
{
    if (cancelled(j)) return -ECANCELED;
    set_current(j, src);
    fr_entry there = {0};
    int exists = !fr_vfs_stat(dst, &there), there_dir = exists && there.is_dir && !there.is_link;
    fr_entry_free(&there);
    int rc = 0;
    if (exists && replace && !(e->is_dir && !e->is_link && there_dir)) {
        rc = remove_tree(NULL, dst, 0);
        if (rc) return fail(j, dst, rc, NULL), rc;
        exists = 0;
    }
    const fr_backend *b = fr_vfs_backend(src);
    if (e->is_link && b->copy_link && fr_uri_same_backend(src, dst)) {
        rc = b->copy_link(src, dst);
        if (rc) fail(j, src, rc, NULL);
        else add_progress(j, e->size, 1);
        return rc;
    }
    if (!e->is_dir) {
        rc = copy_file(j, src, e, dst, replace);
        if (rc && rc != -ECANCELED) fail(j, src, rc, NULL);
        if (!rc) add_progress(j, 0, 1);
        return rc;
    }
    if (!exists && (rc = fr_vfs_mkdir(dst))) return fail(j, dst, rc, NULL), rc;
    add_progress(j, 0, 1);
    fr_entries kids = {0};
    if ((rc = fr_vfs_list(src, &kids))) fail(j, src, rc, NULL);
    int first = 0;
    for (int i = 0; i < kids.n; i++) {
        char *from = fr_uri_join(src, kids.v[i].name), *to = fr_uri_join(dst, kids.v[i].name);
        int r = copy_tree(j, from, &kids.v[i], to, replace);
        free(from), free(to);
        if (r && !first) first = r;
        if (r == -ECANCELED) break;
    }
    fr_entries_free(&kids);
    return rc ? rc : first;
}

/* moves src's contents into the folder dst, replacing what is there; src is removed when all of it moved */
static int merge_move(job *j, const char *src, const char *dst)
{
    fr_entries kids = {0};
    int rc = fr_vfs_list(src, &kids), first = 0;
    if (rc) return fail(j, src, rc, NULL), rc;
    for (int i = 0; i < kids.n && !cancelled(j); i++) {
        char *from = fr_uri_join(src, kids.v[i].name), *to = fr_uri_join(dst, kids.v[i].name);
        fr_entry there = {0};
        int exists = !fr_vfs_stat(to, &there), r = 0;
        if (exists && there.is_dir && !there.is_link && kids.v[i].is_dir && !kids.v[i].is_link) r = merge_move(j, from, to);
        else {
            if (exists) r = remove_tree(NULL, to, 0);
            if (!r) r = fr_vfs_rename(from, to);
            if (r) fail(j, from, r, NULL);
        }
        fr_entry_free(&there);
        free(from), free(to);
        if (r && !first) first = r;
    }
    fr_entries_free(&kids);
    if (cancelled(j)) return -ECANCELED;
    if (!first && (rc = fr_vfs_remove(src))) fail(j, src, rc, NULL), first = rc;
    return first;
}

/* ---- conflicts ---- */

static fr_conflict ask(job *j, const char *dst, int both_dirs)
{
    pthread_mutex_lock(&lock);
    if (j->policy != CONFLICT_ASK) {
        fr_conflict p = j->policy;
        pthread_mutex_unlock(&lock);
        return p;
    }
    char *name = fr_uri_name(dst);
    snprintf(j->st.conflict, sizeof j->st.conflict, "%s", name);
    free(name);
    j->st.conflict_dir = both_dirs;
    j->st.state = JOB_ASKING;
    j->answered = 0;
    wake_locked(1);
    while (!j->answered && !j->cancel) pthread_cond_wait(&work_cond, &lock);
    fr_conflict a = j->cancel ? CONFLICT_SKIP : j->answer;
    j->st.state = JOB_RUNNING;
    pthread_mutex_unlock(&lock);
    return a;
}

/* Where src goes in the destination: NULL to skip it. *replace is set when what is there gives way. */
static char *destination(job *j, const char *src, const fr_entry *e, int *replace)
{
    *replace = 0;
    char *dst = fr_uri_join(j->dest, e->name);
    if (!strcmp(dst, src)) {
        if (j->st.kind == JOB_MOVE) return free(dst), NULL;
        free(dst);
        return fr_uri_free_name(j->dest, e->name, e->is_dir);
    }
    if (e->is_dir && !e->is_link && fr_uri_within(src, j->dest)) {
        fail(j, src, 0, j->st.kind == JOB_MOVE ? "a folder cannot be moved into itself"
                                               : "a folder cannot be copied into itself");
        return free(dst), NULL;
    }
    fr_entry there = {0};
    if (fr_vfs_stat(dst, &there)) return dst;
    int both_dirs = there.is_dir && !there.is_link && e->is_dir && !e->is_link;
    fr_entry_free(&there);
    switch (ask(j, dst, both_dirs)) {
    case CONFLICT_REPLACE: *replace = 1; return dst;
    case CONFLICT_KEEP_BOTH: free(dst); return fr_uri_free_name(j->dest, e->name, e->is_dir);
    default: free(dst); return NULL;
    }
}

/* ---- archives: one pass over the archive for all the items ---- */

static void hook_total(void *c, uint64_t bytes, int files)
{
    job *j = c;
    pthread_mutex_lock(&lock);
    j->st.bytes_total += bytes;
    j->st.files_total += files;
    pthread_mutex_unlock(&lock);
}

static void hook_progress(void *c, uint64_t bytes, int files) { add_progress(c, bytes, files); }

static void hook_current(void *c, const char *name)
{
    job *j = c;
    pthread_mutex_lock(&lock);
    snprintf(j->st.current, sizeof j->st.current, "%s", name);
    wake_locked(0);
    pthread_mutex_unlock(&lock);
}

static void hook_fail(void *c, const char *uri, int err) { fail(c, uri, err, NULL); }

static fr_archive_hooks hooks(job *j) { return (fr_archive_hooks){j, hook_total, hook_progress, hook_current, hook_fail, &j->cancel}; }

static int failures(job *j)
{
    pthread_mutex_lock(&lock);
    int n = j->st.failed;
    pthread_mutex_unlock(&lock);
    return n;
}

/* rc from a whole-archive call: told once, unless the call already told why */
static void archive_done(job *j, const char *uri, int rc, int failed_before)
{
    if (rc && rc != -ECANCELED && failures(j) == failed_before) fail(j, uri, rc, NULL);
}

typedef struct {
    fr_archive_change *v;
    int n, cap;
} changes;

static void rewrite(job *j, const char *outer, changes *ch, fr_strs *owned)
{
    if (ch->n && !cancelled(j)) {
        fr_archive_hooks h = hooks(j);
        int before = failures(j);
        archive_done(j, outer, fr_archive_rewrite(outer, ch->v, ch->n, &h), before);
    }
    free(ch->v);
    *ch = (changes){0};
    fr_strs_free(owned);
}

static const char *own(fr_strs *owned, char *s)
{
    FR_PUSH(*owned, s);
    return s;
}

/* replacing the folder dst (in an archive) by src merges them: what src has that dst has too goes from dst first */
static void merge_deletes(job *j, const char *src, const char *dst, changes *ch, fr_strs *owned)
{
    fr_entries kids = {0};
    if (fr_vfs_list(src, &kids)) return;
    for (int i = 0; i < kids.n && !cancelled(j); i++) {
        char *from = fr_uri_join(src, kids.v[i].name), *to = fr_uri_join(dst, kids.v[i].name);
        fr_entry there = {0};
        if (!fr_vfs_stat(to, &there)) {
            if (there.is_dir && kids.v[i].is_dir && !kids.v[i].is_link) merge_deletes(j, from, to, ch, owned);
            else FR_PUSH(*ch, ((fr_archive_change){.op = FR_AR_DELETE, .path = own(owned, fr_archive_inner(to))}));
        }
        fr_entry_free(&there);
        free(from), free(to);
    }
    fr_entries_free(&kids);
}

/* done[i]: source i is all in its place */
static void copy_into_archive(job *j, const fr_entry *es, int *done)
{
    char *outer = fr_archive_outer(j->dest);
    changes ch = {0};
    fr_strs owned = {0};
    for (int i = 0; i < j->sources.n && !cancelled(j); i++) {
        if (!es[i].name) continue;
        if (fr_uri_within(j->sources.v[i], outer)) {
            fail(j, j->sources.v[i], 0, "an archive cannot be put into itself");
            continue;
        }
        int replace;
        char *dst = destination(j, j->sources.v[i], &es[i], &replace);
        if (!dst) continue;
        const char *path = own(&owned, fr_archive_inner(dst));
        fr_entry there = {0};
        int merge = replace && !fr_vfs_stat(dst, &there) && there.is_dir && es[i].is_dir && !es[i].is_link;
        fr_entry_free(&there);
        if (merge) merge_deletes(j, j->sources.v[i], dst, &ch, &owned);
        else if (replace) FR_PUSH(ch, ((fr_archive_change){.op = FR_AR_DELETE, .path = path}));
        FR_PUSH(ch, ((fr_archive_change){.op = FR_AR_ADD, .path = path, .src = j->sources.v[i]}));
        if (done) done[i] = 1;
        free(dst);
    }
    int before = failures(j);
    rewrite(j, outer, &ch, &owned);
    if (done && (failures(j) != before || cancelled(j)))
        for (int i = 0; i < j->sources.n; i++) done[i] = 0;
    free(outer);
}

static void copy_out_of_archive(job *j, const fr_entry *es, int *done)
{
    char *outer = fr_archive_outer(j->sources.v[0]);
    int n = j->sources.n, k = 0;
    char **from = fr_realloc(NULL, sizeof *from * (size_t)n), **to = fr_realloc(NULL, sizeof *to * (size_t)n);
    int *which = fr_realloc(NULL, sizeof *which * (size_t)n), *failed = fr_realloc(NULL, sizeof *failed * (size_t)n);
    for (int i = 0; i < n && !cancelled(j); i++) {
        if (!es[i].name) continue;
        char *o = fr_archive_outer(j->sources.v[i]);
        int same = o && !strcmp(o, outer);
        free(o);
        if (!same) {
            fail(j, j->sources.v[i], -EXDEV, NULL);
            continue;
        }
        int replace;
        char *dst = destination(j, j->sources.v[i], &es[i], &replace);
        if (!dst) continue;
        fr_entry there = {0};
        int merge = replace && !fr_vfs_stat(dst, &there) && there.is_dir && es[i].is_dir;
        fr_entry_free(&there);
        int rc = replace && !merge ? remove_tree(NULL, dst, 0) : 0;
        if (rc) {
            fail(j, dst, rc, NULL);
            free(dst);
            continue;
        }
        from[k] = fr_archive_inner(j->sources.v[i]), to[k] = dst, which[k++] = i;
    }
    if (k && !cancelled(j)) {
        fr_archive_hooks h = hooks(j);
        int before = failures(j);
        archive_done(j, outer, fr_archive_extract(outer, (const char *const *)from, (const char *const *)to, k, failed, &h), before);
        for (int i = 0; done && i < k; i++) done[which[i]] = !failed[i] && !cancelled(j);
    }
    for (int i = 0; i < k; i++) free(from[i]), free(to[i]);
    free(from), free(to), free(which), free(failed), free(outer);
}

/* sources in one archive: deleted in one rewrite (when only is given, those with only[i] set) */
static void delete_in_archive(job *j, const int *only)
{
    char *outer = fr_archive_outer(j->sources.v[0]);
    changes ch = {0};
    fr_strs owned = {0};
    for (int i = 0; i < j->sources.n; i++) {
        if (only && !only[i]) continue;
        char *o = fr_archive_outer(j->sources.v[i]);
        if (o && !strcmp(o, outer))
            FR_PUSH(ch, ((fr_archive_change){.op = FR_AR_DELETE, .path = own(&owned, fr_archive_inner(j->sources.v[i]))}));
        else fail(j, j->sources.v[i], -EXDEV, NULL);
        free(o);
    }
    rewrite(j, outer, &ch, &owned);
    free(outer);
}

static int involves_archive(job *j)
{
    if (fr_archive_is_uri(j->dest)) return 1;
    for (int i = 0; i < j->sources.n; i++)
        if (fr_archive_is_uri(j->sources.v[i])) return 1;
    return 0;
}

static fr_entry *stat_sources(job *j)
{
    fr_entry *es = fr_realloc(NULL, sizeof *es * (size_t)(j->sources.n ? j->sources.n : 1));
    for (int i = 0; i < j->sources.n; i++) {
        es[i] = (fr_entry){0};
        int rc = fr_vfs_stat(j->sources.v[i], &es[i]);
        if (rc) fail(j, j->sources.v[i], rc, NULL);
    }
    return es;
}

static void free_entries(job *j, fr_entry *es)
{
    for (int i = 0; i < j->sources.n; i++) fr_entry_free(&es[i]);
    free(es);
}

/* copy or move with an archive on either side */
static void copy_archived(job *j, int move)
{
    fr_entry *es = stat_sources(j);
    int n = j->sources.n, *done = fr_realloc(NULL, sizeof *done * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) done[i] = 0;
    int all_inside = fr_archive_is_uri(j->dest), plain = 1;
    for (int i = 0; i < n; i++) all_inside &= fr_uri_same_backend(j->sources.v[i], j->dest);
    if (move && all_inside) {
        /* within one archive a move is a rename, unless a folder merges into one already there */
        char *outer = fr_archive_outer(j->dest);
        changes ch = {0};
        fr_strs owned = {0};
        for (int i = 0; i < n && plain; i++) {
            char *dst = es[i].name ? fr_uri_join(j->dest, es[i].name) : NULL;
            fr_entry there = {0};
            if (dst && strcmp(dst, j->sources.v[i]) && !fr_vfs_stat(dst, &there) && there.is_dir && es[i].is_dir) plain = 0;
            fr_entry_free(&there);
            free(dst);
        }
        for (int i = 0; i < n && plain && !cancelled(j); i++) {
            if (!es[i].name) continue;
            int replace;
            char *dst = destination(j, j->sources.v[i], &es[i], &replace);
            if (!dst) continue;
            const char *path = own(&owned, fr_archive_inner(dst));
            if (replace) FR_PUSH(ch, ((fr_archive_change){.op = FR_AR_DELETE, .path = path}));
            FR_PUSH(ch, ((fr_archive_change){.op = FR_AR_RENAME, .path = own(&owned, fr_archive_inner(j->sources.v[i])), .to = path}));
            free(dst);
        }
        if (plain) rewrite(j, outer, &ch, &owned);
        free(outer);
        if (plain) {
            free(done);
            free_entries(j, es);
            return;
        }
    }
    if (fr_archive_is_uri(j->dest)) copy_into_archive(j, es, done);
    else copy_out_of_archive(j, es, done);
    if (move && !cancelled(j)) {
        if (fr_archive_is_uri(j->sources.v[0])) delete_in_archive(j, done);
        else
            for (int i = 0; i < n; i++) {
                int rc = done[i] ? remove_tree(j, j->sources.v[i], 0) : 0;
                if (rc && rc != -ECANCELED) fail(j, j->sources.v[i], rc, NULL);
            }
    }
    free(done);
    free_entries(j, es);
}

static void run_extract(job *j)
{
    for (int a = 0; a < j->sources.n && !cancelled(j); a++) {
        const char *outer = j->sources.v[a];
        set_current(j, outer);
        char *root = fr_archive_root(outer);
        fr_strs from = {0}, to = {0};
        if (j->flag) {
            char *name = fr_uri_name(outer), *stem = fr_archive_stem(name);
            FR_PUSH(from, fr_strdup(""));
            FR_PUSH(to, fr_uri_free_name(j->dest, *stem ? stem : name, 1));
            free(name), free(stem);
        } else {
            fr_entries top = {0};
            int rc = fr_vfs_list(root, &top);
            if (rc) fail(j, outer, rc, NULL);
            for (int i = 0; i < top.n && !cancelled(j); i++) {
                char *src = fr_uri_join(root, top.v[i].name);
                int replace;
                char *dst = destination(j, src, &top.v[i], &replace);
                fr_entry there = {0};
                int merge = dst && replace && !fr_vfs_stat(dst, &there) && there.is_dir && top.v[i].is_dir;
                fr_entry_free(&there);
                if (dst && replace && !merge && (rc = remove_tree(NULL, dst, 0))) {
                    fail(j, dst, rc, NULL);
                    free(dst);
                    dst = NULL;
                }
                if (dst) {
                    FR_PUSH(from, fr_strdup(top.v[i].name));
                    FR_PUSH(to, dst);
                }
                free(src);
            }
            fr_entries_free(&top);
        }
        if (from.n && !cancelled(j)) {
            fr_archive_hooks h = hooks(j);
            int before = failures(j);
            archive_done(j, outer, fr_archive_extract(outer, (const char *const *)from.v, (const char *const *)to.v, from.n, NULL, &h), before);
        }
        fr_strs_free(&from), fr_strs_free(&to);
        free(root);
    }
}

static void run_compress(job *j)
{
    fr_archive_hooks h = hooks(j);
    int before = failures(j);
    archive_done(j, j->dest, fr_archive_create(j->dest, &j->sources, &h), before);
}

static void run_rename(job *j)
{
    const char *from = j->sources.v[0], *to = j->dest;
    set_current(j, from);
    int rc;
    if (fr_archive_is_uri(from) && fr_uri_same_backend(from, to)) {
        rc = fr_vfs_exists(to) ? -EEXIST : fr_vfs_exists(from) ? 0 : -ENOENT;
        if (!rc) {
            char *outer = fr_archive_outer(from);
            changes ch = {0};
            fr_strs owned = {0};
            FR_PUSH(ch, ((fr_archive_change){.op = FR_AR_RENAME, .path = own(&owned, fr_archive_inner(from)), .to = own(&owned, fr_archive_inner(to))}));
            rewrite(j, outer, &ch, &owned);
            free(outer);
            return;
        }
    } else rc = fr_vfs_rename(from, to);
    if (rc) fail(j, from, rc, NULL);
    else fr_steps_add(&j->log, STEP_RENAME, from, to);
}

static void run_create(job *j)
{
    const char *uri = j->dest;
    set_current(j, uri);
    int rc = 0;
    if (fr_archive_is_uri(uri)) {
        rc = fr_vfs_exists(uri) ? -EEXIST : 0;
        if (!rc) {
            char *outer = fr_archive_outer(uri);
            changes ch = {0};
            fr_strs owned = {0};
            FR_PUSH(ch, ((fr_archive_change){.op = j->flag ? FR_AR_MKDIR : FR_AR_ADD, .path = own(&owned, fr_archive_inner(uri))}));
            rewrite(j, outer, &ch, &owned);
            free(outer);
            return;
        }
    } else if (j->flag) rc = fr_vfs_mkdir(uri);
    else {
        fr_stream *s = fr_vfs_open_write(uri, 1, &rc);
        if (s) rc = s->close(s);
    }
    if (rc) fail(j, uri, rc, NULL);
    else {
        fr_steps_add(&j->log, STEP_CREATED, uri, NULL);
        j->log.v[j->log.n - 1].is_dir = j->flag;
    }
}

static void run_fetch(job *j)
{
    const char *src = j->sources.v[0], *path = fr_uri_local(j->dest);
    set_current(j, src);
    fr_entry e = {0};
    int rc = path ? fr_vfs_stat(src, &e) : -EINVAL;
    if (!rc && e.is_dir) rc = -EISDIR;
    if (!rc && fr_opencache_fresh(path, e.size, e.mtime)) {
        fr_opencache_touch(path);
        add_progress(j, e.size, 1);
    } else if (!rc) {
        pthread_mutex_lock(&lock);
        j->st.bytes_total = e.size, j->st.files_total = 1;
        pthread_mutex_unlock(&lock);
        char *dir = fr_strndup(path, (size_t)(strrchr(path, '/') - path));
        rc = fr_mkdirs(dir);
        free(dir);
        /* the last copy is read-only, so it is taken away rather than written over */
        if (!rc) fr_vfs_remove(j->dest);
        if (!rc) rc = copy_file(j, src, &e, j->dest, 0);
        /* the owner's alone: the source may be a server's private file */
        if (!rc) rc = fr_vfs_set_mode(j->dest, 0400);
        if (!rc) add_progress(j, 0, 1);
    }
    if (rc && rc != -ECANCELED) fail(j, src, rc, NULL);
    fr_entry_free(&e);
}

/* ---- the job kinds ---- */

static void run_copy(job *j)
{
    if (involves_archive(j)) {
        copy_archived(j, 0);
        return;
    }
    fr_entry *es = fr_realloc(NULL, sizeof *es * (size_t)j->sources.n);
    uint64_t bytes = 0;
    int files = 0;
    for (int i = 0; i < j->sources.n; i++) {
        es[i] = (fr_entry){0};
        int rc = fr_vfs_stat(j->sources.v[i], &es[i]);
        if (rc) fail(j, j->sources.v[i], rc, NULL);
        else scan(j, j->sources.v[i], &es[i], &bytes, &files);
    }
    pthread_mutex_lock(&lock);
    j->st.bytes_total = bytes, j->st.files_total = files;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < j->sources.n && !cancelled(j); i++) {
        if (!es[i].name) continue;
        int replace;
        char *dst = destination(j, j->sources.v[i], &es[i], &replace);
        if (dst) {
            copy_tree(j, j->sources.v[i], &es[i], dst, replace);
            if (!replace && fr_vfs_exists(dst)) fr_steps_add(&j->log, STEP_COPIED, j->sources.v[i], dst);
        }
        free(dst);
    }
    for (int i = 0; i < j->sources.n; i++) fr_entry_free(&es[i]);
    free(es);
}

static void run_move(job *j)
{
    if (involves_archive(j)) {
        copy_archived(j, 1);
        return;
    }
    pthread_mutex_lock(&lock);
    j->st.files_total = j->sources.n;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < j->sources.n && !cancelled(j); i++) {
        const char *src = j->sources.v[i];
        fr_entry e = {0};
        int rc = fr_vfs_stat(src, &e);
        if (rc) {
            fail(j, src, rc, NULL);
            continue;
        }
        set_current(j, src);
        int replace;
        char *dst = destination(j, src, &e, &replace);
        if (dst) {
            fr_entry there = {0};
            int merge = replace && !fr_vfs_stat(dst, &there) && there.is_dir && !there.is_link && e.is_dir && !e.is_link;
            fr_entry_free(&there);
            if (merge && fr_uri_same_backend(src, dst)) rc = merge_move(j, src, dst);
            else {
                rc = replace && !merge ? remove_tree(NULL, dst, 0) : 0;
                if (!rc) rc = merge ? -EXDEV : fr_vfs_rename(src, dst);
                /* another device: copy, then delete what was copied */
                if (rc == -EXDEV || rc == -ENOTSUP || rc == -EPROTONOSUPPORT) {
                    uint64_t bytes = 0;
                    int files = 0;
                    scan(j, src, &e, &bytes, &files);
                    pthread_mutex_lock(&lock);
                    j->st.bytes_total += bytes;
                    pthread_mutex_unlock(&lock);
                    rc = copy_tree(j, src, &e, dst, replace);
                    if (!rc && (rc = remove_tree(j, src, 0))) fail(j, src, rc, NULL);
                } else if (rc) fail(j, src, rc, NULL);
                if (!rc && !merge) fr_steps_add(&j->log, STEP_RENAME, src, dst);
            }
            if (!rc) add_progress(j, 0, 1);
        }
        free(dst);
        fr_entry_free(&e);
    }
}

static void run_delete(job *j)
{
    if (j->sources.n && fr_archive_is_uri(j->sources.v[0])) {
        delete_in_archive(j, NULL);
        return;
    }
    int files = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < j->sources.n && !cancelled(j); i++) {
        fr_entry e = {0};
        if (!fr_vfs_stat(j->sources.v[i], &e)) scan(j, j->sources.v[i], &e, &bytes, &files);
        fr_entry_free(&e);
    }
    pthread_mutex_lock(&lock);
    j->st.files_total = files;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < j->sources.n && !cancelled(j); i++) {
        set_current(j, j->sources.v[i]);
        int rc = remove_tree(j, j->sources.v[i], 1);
        if (rc && rc != -ECANCELED) fail(j, j->sources.v[i], rc, NULL);
    }
}

static void run_trash(job *j)
{
    pthread_mutex_lock(&lock);
    j->st.files_total = j->sources.n;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < j->sources.n && !cancelled(j); i++) {
        set_current(j, j->sources.v[i]);
        int rc = fr_vfs_trash(j->sources.v[i]);
        if (rc) fail(j, j->sources.v[i], rc, rc == -ENOTSUP ? "cannot be moved to the trash here" : NULL);
        else {
            fr_steps_add(&j->log, STEP_TRASH, j->sources.v[i], NULL);
            add_progress(j, 0, 1);
        }
    }
}

static void chmod_tree(job *j, const char *uri, const fr_entry *e, int top)
{
    if (cancelled(j)) return;
    if (e->is_link) {
        add_progress(j, 0, 1);
        return;
    }
    set_current(j, uri);
    int folder = e->is_dir;
    uint32_t mode = fr_mode_edit(e->mode, j->mode_set, j->mode_clear, folder && !top);
    int rc;
    if (folder) {
        /* the owner keeps reading and searching the folder until what is inside is done */
        if ((e->mode & 0500) != 0500 || (mode & 0500) != 0500) fr_vfs_set_mode(uri, mode | 0500);
        fr_entries kids = {0};
        if ((rc = fr_vfs_list(uri, &kids))) fail(j, uri, rc, NULL);
        for (int i = 0; i < kids.n && !cancelled(j); i++) {
            char *child = fr_uri_join(uri, kids.v[i].name);
            chmod_tree(j, child, &kids.v[i], 0);
            free(child);
        }
        fr_entries_free(&kids);
    }
    if ((rc = fr_vfs_set_mode(uri, mode))) fail(j, uri, rc, NULL);
    else {
        if (mode != e->mode) fr_steps_add_chmod(&j->log, uri, e->mode, mode);
        add_progress(j, 0, 1);
    }
}

static void run_chmod(job *j)
{
    int files = 0;
    uint64_t bytes = 0;
    fr_entry *es = fr_realloc(NULL, sizeof *es * (size_t)j->sources.n);
    for (int i = 0; i < j->sources.n; i++) {
        es[i] = (fr_entry){0};
        int rc = fr_vfs_stat(j->sources.v[i], &es[i]);
        if (rc) fail(j, j->sources.v[i], rc, NULL);
        else if (!cancelled(j)) scan(j, j->sources.v[i], &es[i], &bytes, &files);
    }
    pthread_mutex_lock(&lock);
    j->st.files_total = files;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < j->sources.n && !cancelled(j); i++)
        if (es[i].name) chmod_tree(j, j->sources.v[i], &es[i], 1);
    for (int i = 0; i < j->sources.n; i++) fr_entry_free(&es[i]);
    free(es);
}

/* a rename, else a copy and a delete where the two are on different devices; reports its own failure */
static int move_item(job *j, const char *from, const char *to)
{
    int rc = fr_vfs_rename(from, to);
    if (rc == -EXDEV || rc == -ENOTSUP || rc == -EPROTONOSUPPORT) {
        fr_entry e = {0};
        if (fr_vfs_exists(to)) rc = -EEXIST;
        else if (!(rc = fr_vfs_stat(from, &e))) {
            uint64_t bytes = 0;
            int files = 0;
            scan(j, from, &e, &bytes, &files);
            pthread_mutex_lock(&lock);
            j->st.bytes_total += bytes;
            pthread_mutex_unlock(&lock);
            if ((rc = copy_tree(j, from, &e, to, 0))) return fr_entry_free(&e), rc;
            rc = remove_tree(j, from, 0);
        }
        fr_entry_free(&e);
    }
    if (rc && rc != -ECANCELED) fail(j, from, rc, NULL);
    return rc;
}

/* an empty file or folder */
static int make_empty(const char *uri, int is_dir)
{
    if (is_dir) return fr_vfs_mkdir(uri);
    int rc = 0;
    fr_stream *s = fr_vfs_open_write(uri, 1, &rc);
    return s ? s->close(s) : rc;
}

static int remove_empty(const char *uri)
{
    fr_entry e = {0};
    int rc = fr_vfs_stat(uri, &e);
    if (!rc && !e.is_dir && e.size) rc = -ENOTEMPTY;
    fr_entry_free(&e);
    return rc ? rc : fr_vfs_remove(uri);
}

static void run_steps(job *j)
{
    pthread_mutex_lock(&lock);
    j->st.files_total = j->todo.n;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < j->todo.n && !cancelled(j); i++) {
        const fr_step *s = &j->todo.v[i];
        set_current(j, s->a);
        fr_step_kind done = s->kind;
        int rc = -ENOTSUP;
        switch (s->kind) {
        case STEP_RENAME:
            if ((rc = move_item(j, s->a, s->b))) continue;
            break;
        case STEP_TRASH:
            rc = fr_vfs_trash(s->a);
            if (rc == -ENOTSUP && s->may_remove) rc = remove_empty(s->a), done = STEP_REMOVED;
            break;
        case STEP_UNTRASH: rc = fr_vfs_untrash(s->a); break;
        case STEP_CREATED: rc = make_empty(s->a, s->is_dir); break;
        case STEP_REMOVED: rc = remove_empty(s->a); break;
        case STEP_CHMOD: rc = fr_vfs_set_mode(s->a, s->mode); break;
        case STEP_COPIED: break;
        }
        if (rc) {
            fail(j, s->a, rc, rc == -ENOTSUP && s->kind == STEP_TRASH ? "there is no trash here" : NULL);
            continue;
        }
        fr_step d = *s;
        d.kind = done, d.a = fr_strdup(s->a), d.b = s->b ? fr_strdup(s->b) : NULL;
        FR_PUSH(j->log, d);
        add_progress(j, 0, 1);
    }
}

static void *work(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    while (!stopping) {
        job *j = NULL;
        for (int i = 0; i < jobs.n && !j; i++)
            if (jobs.v[i]->st.state == JOB_QUEUED) j = jobs.v[i];
        if (!j) {
            pthread_cond_broadcast(&idle_cond);
            pthread_cond_wait(&work_cond, &lock);
            continue;
        }
        j->st.state = JOB_RUNNING;
        wake_locked(1);
        pthread_mutex_unlock(&lock);
        switch (j->st.kind) {
        case JOB_COPY: run_copy(j); break;
        case JOB_MOVE: run_move(j); break;
        case JOB_DELETE: run_delete(j); break;
        case JOB_TRASH: run_trash(j); break;
        case JOB_CHMOD: run_chmod(j); break;
        case JOB_EXTRACT: run_extract(j); break;
        case JOB_COMPRESS: run_compress(j); break;
        case JOB_RENAME: run_rename(j); break;
        case JOB_CREATE: run_create(j); break;
        case JOB_STEPS: run_steps(j); break;
        case JOB_FETCH: run_fetch(j); break;
        }
        pthread_mutex_lock(&lock);
        j->st.state = j->cancel ? JOB_CANCELLED : JOB_DONE;
        j->st.current[0] = 0;
        finished++;
        wake_locked(1);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

void fr_jobs_start(void (*wake)(void))
{
    pthread_mutex_lock(&lock);
    wake_fn = wake;
    if (!started) started = !pthread_create(&worker, NULL, work, NULL);
    pthread_mutex_unlock(&lock);
}

static void job_free(job *j)
{
    fr_strs_free(&j->sources);
    free(j->dest);
    fr_steps_free(&j->todo);
    fr_steps_free(&j->log);
    free(j);
}

void fr_jobs_stop(void)
{
    pthread_mutex_lock(&lock);
    stopping = 1;
    for (int i = 0; i < jobs.n; i++) __atomic_store_n(&jobs.v[i]->cancel, 1, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&work_cond);
    pthread_mutex_unlock(&lock);
    if (started) pthread_join(worker, NULL);
    for (int i = 0; i < jobs.n; i++) job_free(jobs.v[i]);
    free(jobs.v);
    jobs.v = NULL, jobs.n = jobs.cap = 0;
    started = stopping = 0;
}

static job *job_new(fr_job_kind kind, const fr_strs *sources, const char *dest_dir)
{
    job *j = fr_realloc(NULL, sizeof *j);
    *j = (job){0};
    j->sources = fr_strs_copy(sources);
    j->dest = fr_strdup(dest_dir ? dest_dir : "");
    j->st.kind = kind;
    j->st.items = sources->n;
    if (dest_dir) {
        char *name = fr_uri_name(dest_dir);
        snprintf(j->st.dest_name, sizeof j->st.dest_name, "%s", *name ? name : "/");
        free(name);
    }
    return j;
}

static int job_queue(job *j)
{
    pthread_mutex_lock(&lock);
    j->st.id = next_id++;
    FR_PUSH(jobs, j);
    pthread_cond_broadcast(&work_cond);
    int id = j->st.id;
    pthread_mutex_unlock(&lock);
    return id;
}

int fr_job_submit(fr_job_kind kind, const fr_strs *sources, const char *dest_dir)
{
    return job_queue(job_new(kind, sources, dest_dir));
}

int fr_job_submit_chmod(const fr_strs *sources, uint32_t set, uint32_t clear)
{
    job *j = job_new(JOB_CHMOD, sources, NULL);
    j->mode_set = set & 07777, j->mode_clear = clear & 07777;
    return job_queue(j);
}

int fr_job_submit_steps(const fr_steps *steps, const char *what)
{
    fr_strs none = {0};
    job *j = job_new(JOB_STEPS, &none, NULL);
    j->todo = fr_steps_copy(steps);
    j->st.items = steps->n;
    snprintf(j->st.what, sizeof j->st.what, "%s", what);
    return job_queue(j);
}

int fr_job_take_log(int id, fr_steps *out)
{
    pthread_mutex_lock(&lock);
    job *j = NULL;
    for (int i = 0; i < jobs.n && !j; i++)
        if (jobs.v[i]->st.id == id) j = jobs.v[i];
    int rc = -1;
    if (j && (j->st.state == JOB_DONE || j->st.state == JOB_CANCELLED)) {
        *out = j->log;
        j->log = (fr_steps){0};
        rc = 0;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

int fr_job_submit_extract(const fr_strs *archives, const char *dest_dir, int into_folder)
{
    job *j = job_new(JOB_EXTRACT, archives, dest_dir);
    j->flag = into_folder;
    return job_queue(j);
}

int fr_job_submit_compress(const fr_strs *sources, const char *archive_uri)
{
    return job_queue(job_new(JOB_COMPRESS, sources, archive_uri));
}

int fr_job_submit_rename(const char *from, const char *to)
{
    fr_strs one = {(char *[]){(char *)from}, 1, 1};
    return job_queue(job_new(JOB_RENAME, &one, to));
}

int fr_job_submit_create(const char *uri, int folder)
{
    fr_strs one = {(char *[]){(char *)uri}, 1, 1};
    job *j = job_new(JOB_CREATE, &one, uri);
    j->flag = folder;
    return job_queue(j);
}

int fr_job_submit_fetch(const char *uri, const char *copy)
{
    fr_strs one = {(char *[]){(char *)uri}, 1, 1};
    job *j = job_new(JOB_FETCH, &one, copy);
    char *name = fr_uri_name(uri);
    snprintf(j->st.dest_name, sizeof j->st.dest_name, "%s", name);
    free(name);
    return job_queue(j);
}

int fr_jobs_list(fr_job_status *out, int max)
{
    pthread_mutex_lock(&lock);
    int n = 0;
    for (int i = 0; i < jobs.n && n < max; i++) out[n++] = jobs.v[i]->st;
    pthread_mutex_unlock(&lock);
    return n;
}

static job *find(int id)
{
    for (int i = 0; i < jobs.n; i++)
        if (jobs.v[i]->st.id == id) return jobs.v[i];
    return NULL;
}

void fr_job_answer(int id, fr_conflict answer, int for_all)
{
    pthread_mutex_lock(&lock);
    job *j = find(id);
    if (j) {
        j->answer = answer, j->answered = 1;
        if (for_all) j->policy = answer;
        pthread_cond_broadcast(&work_cond);
    }
    pthread_mutex_unlock(&lock);
}

void fr_job_cancel(int id)
{
    pthread_mutex_lock(&lock);
    job *j = find(id);
    if (j) {
        __atomic_store_n(&j->cancel, 1, __ATOMIC_RELAXED);
        if (j->st.state == JOB_QUEUED) j->st.state = JOB_CANCELLED, finished++;
        pthread_cond_broadcast(&work_cond);
    }
    pthread_mutex_unlock(&lock);
}

void fr_job_dismiss(int id)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < jobs.n; i++) {
        job *j = jobs.v[i];
        if (j->st.id != id || (j->st.state != JOB_DONE && j->st.state != JOB_CANCELLED)) continue;
        job_free(j);
        memmove(jobs.v + i, jobs.v + i + 1, sizeof *jobs.v * (size_t)(jobs.n - i - 1));
        jobs.n--;
        break;
    }
    pthread_mutex_unlock(&lock);
}

int fr_jobs_finished(void)
{
    pthread_mutex_lock(&lock);
    int n = finished;
    pthread_mutex_unlock(&lock);
    return n;
}

void fr_jobs_wait_idle(void)
{
    pthread_mutex_lock(&lock);
    for (;;) {
        int busy = 0;
        for (int i = 0; i < jobs.n; i++)
            if (jobs.v[i]->st.state <= JOB_ASKING) busy = 1;
        if (!busy) break;
        pthread_cond_wait(&idle_cond, &lock);
    }
    pthread_mutex_unlock(&lock);
}

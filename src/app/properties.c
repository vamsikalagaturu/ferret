#define _GNU_SOURCE
#include "app.h"
#include "core/hash.h"
#include "jobs/async.h"
#include "vfs/local_info.h"
#include <errno.h>
#include <strings.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* folders are counted on a thread of their own; the panel may close (and drop its reference) before it ends */
typedef struct {
    fr_strs uris;
    fr_tree_count c;
    int cancel, done, refs;
} count_task;

/* MD5, SHA-1 and SHA-256 of one file in a single read, on a thread of its own, shared with the panel the same way */
typedef struct {
    char *uri;
    uint64_t done, total;
    int cancel, finished, err, refs;
    char hex[3][65];
} hash_task;

enum { BIT_OFF, BIT_ON, BIT_MIXED };
static const char *const classes[3] = {"Owner", "Group", "Others"};
static const char *const perms[3] = {"Read", "Write", "Execute"};

static struct {
    int open;
    fr_strs uris;
    fr_entry *ents;       /* per URI; name NULL when it could not be looked at */
    int folders, files, links, missing;
    int local;            /* single item on a local path: info is filled */
    fr_local_info info;
    char path[4096];      /* single: the item; several: the folder holding them */
    int editable;         /* items with permission bits of their own (not links) */
    int can_set;          /* the storage changes permission bits */
    uint32_t all_on, any_on; /* over the editable items */
    int bit[9], orig[9];
    int recursive;
    fr_perm_only only;  /* recursive: what inside gets the bits */
    int exec_x;         /* chmod's X for the execute bits set */
    int special;        /* the setuid, setgid and sticky bits typed in the octal field; -1 as they are */
    fr_text octal;
    int all_local;      /* every editable item is on a local path: owner and group can change */
    int64_t uid, gid;   /* theirs when they share it, -1 mixed */
    fr_text owner, group;
    char owner_was[256], group_was[256];
    char error[512];
    int has_space;
    uint64_t space_free, space_total;
    int has_mount;
    fr_local_info folder_info; /* several: of the folder holding them */
    count_task *count;
    hash_task *hash;
    int compared; /* 0 not yet, 1 + the matching hash, -1 none matched, -2 the clipboard was empty */
    fr_scroll scroll;
    float content_h;
    int loading;  /* the items' details are on their way from off this device */
} P;

static uint32_t bit_mask(int i) { return 1u << (8 - i); }

/* ---- counting ---- */

static void task_drop(count_task *t)
{
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL)) return;
    fr_strs_free(&t->uris);
    free(t);
}

static void *count_run(void *arg)
{
    count_task *t = arg;
    for (int i = 0; i < t->uris.n && !__atomic_load_n(&t->cancel, __ATOMIC_RELAXED); i++)
        fr_vfs_measure(t->uris.v[i], &t->c, &t->cancel);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    plat_wake();
    task_drop(t);
    return NULL;
}

static void count_start(void)
{
    count_task *t = fr_realloc(NULL, sizeof *t);
    *t = (count_task){.uris = fr_strs_copy(&P.uris), .refs = 2};
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, count_run, t)) {
        t->refs = 1;
        task_drop(t);
        t = NULL;
    }
    pthread_attr_destroy(&attr);
    P.count = t;
}

static void count_stop(void)
{
    if (!P.count) return;
    __atomic_store_n(&P.count->cancel, 1, __ATOMIC_RELAXED);
    task_drop(P.count);
    P.count = NULL;
}

/* ---- checksums ---- */

static void hash_drop(hash_task *t)
{
    if (__atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL)) return;
    free(t->uri);
    free(t);
}

static void *hash_run(void *arg)
{
    hash_task *t = arg;
    static const fr_hash_kind kinds[3] = {FR_MD5, FR_SHA1, FR_SHA256};
    fr_hash h[3];
    for (int i = 0; i < 3; i++) fr_hash_init(&h[i], kinds[i]);
    int err = 0;
    fr_stream *s = fr_vfs_open_read(t->uri, &err);
    char *buf = fr_realloc(NULL, 256 * 1024);
    while (s && !err) {
        if (__atomic_load_n(&t->cancel, __ATOMIC_RELAXED)) err = -ECANCELED;
        long n = err ? 0 : s->read(s, buf, 256 * 1024);
        if (n < 0) err = (int)n;
        if (n <= 0) break;
        for (int i = 0; i < 3; i++) fr_hash_update(&h[i], buf, (size_t)n);
        __atomic_add_fetch(&t->done, (uint64_t)n, __ATOMIC_RELAXED);
    }
    if (s) s->close(s);
    free(buf);
    if (!err)
        for (int i = 0; i < 3; i++) fr_hash_final_hex(&h[i], t->hex[i]);
    t->err = err;
    __atomic_store_n(&t->finished, 1, __ATOMIC_RELEASE);
    plat_wake();
    hash_drop(t);
    return NULL;
}

static void hash_start(void)
{
    hash_task *t = fr_realloc(NULL, sizeof *t);
    *t = (hash_task){.uri = fr_strdup(P.uris.v[0]), .total = P.ents[0].size, .refs = 2};
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, hash_run, t)) {
        t->err = -EAGAIN, t->finished = 1, t->refs = 1;
    }
    pthread_attr_destroy(&attr);
    P.hash = t;
    P.compared = 0;
}

static void hash_stop(void)
{
    if (!P.hash) return;
    __atomic_store_n(&P.hash->cancel, 1, __ATOMIC_RELAXED);
    hash_drop(P.hash);
    P.hash = NULL;
}

/* the clipboard's first word (sha256sum prints "<hash>  <file>"), in any case, against the three */
static void compare_clipboard(void)
{
    char *clip = plat_clipboard();
    P.compared = -2;
    if (clip) {
        char *s = clip;
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
        size_t n = strcspn(s, " \t\r\n");
        P.compared = n ? -1 : -2;
        for (int i = 0; n && i < 3; i++)
            if (n == strlen(P.hash->hex[i]) && !strncasecmp(s, P.hash->hex[i], n)) P.compared = 1 + i;
    }
    free(clip);
}

/* ---- what the panel shows ---- */

static void load_modes(void)
{
    P.editable = 0, P.all_on = 07777, P.any_on = 0;
    for (int i = 0; i < P.uris.n; i++) {
        fr_entry *e = &P.ents[i];
        if (!e->name || e->is_link) continue;
        P.editable++;
        P.all_on &= e->mode, P.any_on |= e->mode;
    }
    if (!P.editable) P.all_on = 0;
    for (int i = 0; i < 9; i++) {
        uint32_t b = bit_mask(i);
        P.bit[i] = P.orig[i] = P.all_on & b ? BIT_ON : P.any_on & b ? BIT_MIXED : BIT_OFF;
    }
    P.special = -1;
    P.all_local = P.editable > 0;
    P.uid = P.gid = -2;
    for (int i = 0; i < P.uris.n; i++) {
        fr_entry *e = &P.ents[i];
        if (!e->name || e->is_link) continue;
        const char *path = fr_uri_local(P.uris.v[i]);
        struct stat st;
        if (!path || lstat(path, &st)) {
            P.all_local = 0;
            continue;
        }
        P.uid = P.uid == -2 || P.uid == (int64_t)st.st_uid ? (int64_t)st.st_uid : -1;
        P.gid = P.gid == -2 || P.gid == (int64_t)st.st_gid ? (int64_t)st.st_gid : -1;
    }
    char name[256] = "";
    if (P.uid >= 0) fr_owner_name((uint32_t)P.uid, 0, name, sizeof name);
    if (P.uid >= 0 && !*name) snprintf(name, sizeof name, "%lld", (long long)P.uid);
    snprintf(P.owner_was, sizeof P.owner_was, "%s", P.uid >= 0 ? name : "");
    name[0] = 0;
    if (P.gid >= 0) fr_owner_name((uint32_t)P.gid, 1, name, sizeof name);
    if (P.gid >= 0 && !*name) snprintf(name, sizeof name, "%lld", (long long)P.gid);
    snprintf(P.group_was, sizeof P.group_was, "%s", P.gid >= 0 ? name : "");
    text_set(&P.owner, P.owner_was);
    text_set(&P.group, P.group_was);
}

/* What the storage says of the items: on the window's thread for this device's files, else on a thread of its own
 * (jobs/async.h), since a server may take its time. */
typedef struct {
    unsigned serial; /* the panel's load it is for */
    fr_strs uris;
    char *where;     /* the item, or the folder holding several */
    fr_entry *ents;
    int first_err;   /* of the first stat that failed */
    int has_space;
    uint64_t space_free, space_total;
} gathered;

static unsigned load_serial;

static void gather(gathered *g)
{
    g->ents = fr_realloc(NULL, sizeof *g->ents * (size_t)g->uris.n);
    for (int i = 0; i < g->uris.n; i++) {
        g->ents[i] = (fr_entry){0};
        int rc = fr_vfs_stat(g->uris.v[i], &g->ents[i]);
        if (rc) {
            fr_entry_free(&g->ents[i]);
            g->ents[i] = (fr_entry){0};
            if (!g->first_err) g->first_err = rc;
        }
    }
    const fr_backend *b = fr_vfs_backend(g->where);
    g->has_space = b && b->free_space && !b->free_space(g->where, &g->space_free, &g->space_total);
}

static void gathered_free(gathered *g)
{
    for (int i = 0; g->ents && i < g->uris.n; i++) fr_entry_free(&g->ents[i]);
    free(g->ents);
    fr_strs_free(&g->uris);
    free(g->where);
    free(g);
}

static gathered *gathered_new(void)
{
    gathered *g = fr_realloc(NULL, sizeof *g);
    char *parent = fr_uri_parent(P.uris.v[0]);
    *g = (gathered){.serial = load_serial, .uris = fr_strs_copy(&P.uris)};
    g->where = fr_strdup(P.uris.n == 1 || !parent ? P.uris.v[0] : parent);
    free(parent);
    return g;
}

/* takes g's entries */
static void install(gathered *g)
{
    for (int i = 0; P.ents && i < P.uris.n; i++) fr_entry_free(&P.ents[i]);
    free(P.ents);
    P.ents = g->ents;
    g->ents = NULL;
    P.folders = P.files = P.links = P.missing = 0;
    for (int i = 0; i < P.uris.n; i++) {
        fr_entry *e = &P.ents[i];
        if (!e->name) P.missing++;
        else if (e->is_link) P.links++;
        else if (e->is_dir) P.folders++;
        else P.files++;
    }
    load_modes();
    const fr_backend *b = fr_vfs_backend(P.uris.v[0]);
    P.can_set = b && b->set_mode;
    const char *local = fr_uri_local(P.uris.v[0]), *where_local = fr_uri_local(g->where);
    snprintf(P.path, sizeof P.path, "%s", where_local ? where_local : g->where);
    P.local = P.uris.n == 1 && local && !fr_local_info_get(local, &P.info);
    P.has_mount = 0;
    if (P.local) P.has_mount = P.info.has_mount;
    else if (where_local && !fr_local_info_get(where_local, &P.folder_info)) P.has_mount = P.folder_info.has_mount;
    P.has_space = g->has_space, P.space_free = g->space_free, P.space_total = g->space_total;
    P.loading = 0;
    ui.dirty = 1;
}

static void gather_work(void *arg) { gather(arg); }

static void props_retry(void);

static void gather_done(void *arg)
{
    gathered *g = arg;
    if (P.open && g->serial == load_serial) {
        int first = !P.ents;
        if (g->first_err && !P.ents && net_ask(g->uris.v[0], g->first_err, props_retry)) {
            gathered_free(g);
            return;
        }
        install(g);
        if (first && (P.uris.n > 1 || P.folders)) count_start();
    }
    gathered_free(g);
}

static void load(void)
{
    load_serial++;
    gathered *g = gathered_new();
    if (fr_uri_local(P.uris.v[0])) {
        gather(g);
        install(g);
        gathered_free(g);
        return;
    }
    P.loading = 1;
    net_prepare(P.uris.v[0]);
    fr_async_run(gather_work, gather_done, g);
}

static void props_retry(void)
{
    if (P.open && P.loading) load();
}

void props_open(fr_strs *uris)
{
    props_close();
    P.uris = *uris;
    *uris = (fr_strs){0};
    P.open = 1;
    load();
    if (!P.loading && (P.uris.n > 1 || P.folders)) count_start();
}

void props_close(void)
{
    if (!P.open) return;
    count_stop();
    hash_stop();
    for (int i = 0; i < P.uris.n; i++) fr_entry_free(&P.ents[i]);
    free(P.ents);
    fr_strs_free(&P.uris);
    memset(&P, 0, sizeof P);
    /* a field of the panel's must not keep the keys from the list */
    ui.focus = 0;
    ui.dirty = 1;
}

/* ---- formatting ---- */

static void fmt_int(uint64_t v, char *out, size_t len)
{
    char digits[32];
    int n = snprintf(digits, sizeof digits, "%llu", (unsigned long long)v), k = 0;
    for (int i = 0; i < n && (size_t)k + 2 < len; i++) {
        if (i && (n - i) % 3 == 0) out[k++] = ',';
        out[k++] = digits[i];
    }
    out[k] = 0;
}

static void fmt_size(uint64_t bytes, char *out, size_t len)
{
    char human[32], exact[32];
    fr_format_size(bytes, human, sizeof human);
    if (bytes < 1000) {
        snprintf(out, len, "%s", human);
        return;
    }
    fmt_int(bytes, exact, sizeof exact);
    snprintf(out, len, "%s (%s bytes)", human, exact);
}

static void fmt_date(int64_t t, char *out, size_t len)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(out, len, "%Y-%m-%d %H:%M:%S %z", &tm);
}

static void fmt_id(const char *name, uint32_t id, char *out, size_t len)
{
    if (*name) snprintf(out, len, "%s (%u)", name, id);
    else snprintf(out, len, "%u", id);
}

static const char *plural(int64_t n, const char *one, const char *many) { return n == 1 ? one : many; }

/* "rwxr-x---" and "0750" of what the grid says; a ? where it is mixed */
static void grid_mode(char rwx[10], char octal[6])
{
    uint32_t m = 0;
    int mixed_digit[3] = {0};
    for (int i = 0; i < 9; i++) {
        if (P.bit[i] == BIT_ON) m |= bit_mask(i);
        if (P.bit[i] == BIT_MIXED) mixed_digit[i / 3] = 1;
    }
    uint32_t special = P.special >= 0 ? (uint32_t)P.special : P.all_on & 07000;
    fr_mode_rwx(m | special, rwx);
    fr_mode_octal(m | special, octal);
    for (int i = 0; i < 9; i++)
        if (P.bit[i] == BIT_MIXED) rwx[i] = '?';
    for (int d = 0; d < 3; d++)
        if (mixed_digit[d]) octal[1 + d] = '?';
    if (P.special < 0 && (P.any_on & 07000) != special) octal[0] = '?';
}

static void special_text(char *out, size_t len)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } names[] = {{04000, "setuid"}, {02000, "setgid"}, {01000, "sticky"}};
    size_t k = 0;
    out[0] = 0;
    uint32_t any = P.special >= 0 ? (uint32_t)P.special : P.any_on, all = P.special >= 0 ? (uint32_t)P.special : P.all_on;
    for (int i = 0; i < 3; i++) {
        if (!(any & names[i].bit)) continue;
        k += (size_t)snprintf(out + k, len - k, "%s%s%s", k ? ", " : "", names[i].name, all & names[i].bit ? "" : " (some)");
        if (k >= len) break;
    }
    if (!out[0]) snprintf(out, len, "None");
}

/* ---- layout ---- */

static struct {
    float x, y, w, lw, top;
    Rectangle body;
} L;

static float row_h(void) { return DP(ui.touch ? 32 : 26); }

/* the part of r inside the scrolled body, so what scrolled out of view takes no clicks */
static Rectangle in_body(Rectangle r)
{
    float x0 = fmaxf(r.x, L.body.x), y0 = fmaxf(r.y, L.body.y);
    float x1 = fminf(r.x + r.width, L.body.x + L.body.width), y1 = fminf(r.y + r.height, L.body.y + L.body.height);
    return (Rectangle){x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0)};
}

static void section(const char *title)
{
    if (L.y > L.top) L.y += DP(14);
    ui_text_box(title, (Rectangle){L.x, L.y, L.w, DP(24)}, 13, FONT_SEMIBOLD, theme.accent, ALIGN_LEFT, 0);
    L.y += DP(26);
    ui_hline(L.x, L.y, L.w, theme.border);
    L.y += DP(6);
}

static void label(const char *s, float h) { ui_text_box(s, (Rectangle){L.x, L.y, L.lw - DP(12), h}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0); }

static void row_c(const char *name, const char *value, Color c)
{
    float h = row_h();
    label(name, h);
    ui_text_box(value, (Rectangle){L.x + L.lw, L.y, L.w - L.lw, h}, 14, FONT_REGULAR, c, ALIGN_LEFT, 0);
    L.y += h;
}

static void row(const char *name, const char *value) { row_c(name, value, theme.text); }

/* a value that wraps onto as many lines as it needs, with a button copying it when copy is set */
static void row_long(const char *name, const char *value, int copy)
{
    float h = row_h(), lh = ui_line_height(14), btn = DP(28), vw = L.w - L.lw - (copy ? btn + DP(8) : 0);
    int lines = ui_text_wrapped(value, (Rectangle){0, 0, vw, 0}, 14, FONT_REGULAR, BLANK, 0);
    float top = (h - (float)ui_font(FONT_REGULAR, 14).baseSize) / 2;
    label(name, h);
    ui_text_wrapped(value, (Rectangle){L.x + L.lw, L.y + top, vw, 0}, 14, FONT_REGULAR, theme.text, 1);
    if (copy) {
        Rectangle b = {L.x + L.w - btn, L.y + (h - btn) / 2, btn, btn};
        int state;
        if (ui_clickable(ui_id("props-copy", copy), in_body(b), &state)) {
            plat_set_clipboard(value);
            ui_toast("Copied %s", value);
        }
        if (state) ui_round(b, 6, state == 2 ? theme.pressed : theme.hover);
        ui_icon("copy", b, 16, theme.text_dim);
    }
    L.y += fmaxf(h, (lines - 1) * lh + h);
}

static int checkbox(unsigned id, Rectangle cell, int state, int enabled)
{
    Rectangle hit = {cell.x + (cell.width - DP(32)) / 2, cell.y + (cell.height - DP(32)) / 2, DP(32), DP(32)};
    int s = 0, clicked = enabled && ui_clickable(id, in_body(hit), &s);
    if (enabled && s) ui_round(hit, 6, s == 2 ? theme.pressed : theme.hover);
    const char *icon = state == BIT_ON ? "square-check" : state == BIT_MIXED ? "square-minus" : "square";
    Color c = !enabled ? theme.text_faint : state == BIT_OFF ? theme.text_dim : theme.accent;
    ui_icon(icon, cell, 20, c);
    return clicked;
}

/* ---- the sections ---- */

static void general(void)
{
    char buf[256], a[64], b[64];
    section("General");
    count_task *t = P.count;
    int counting = t && !__atomic_load_n(&t->done, __ATOMIC_ACQUIRE);
    int64_t files = t ? __atomic_load_n(&t->c.files, __ATOMIC_RELAXED) : 0;
    int64_t folders = t ? __atomic_load_n(&t->c.folders, __ATOMIC_RELAXED) : 0;
    int64_t unreadable = t ? __atomic_load_n(&t->c.unreadable, __ATOMIC_RELAXED) : 0;
    uint64_t bytes = t ? __atomic_load_n(&t->c.bytes, __ATOMIC_RELAXED) : 0;
    if (P.uris.n == 1 && !P.ents[0].name) {
        row_c("Kind", "Gone, or could not be looked at", theme.warning);
        row_long("Path", P.path, 1);
    } else if (P.uris.n == 1) {
        fr_entry *e = &P.ents[0];
        fr_kind k = kind_of(e->name, e->is_dir);
        snprintf(buf, sizeof buf, "%s%s", e->is_link ? "Link to a " : "", e->is_link ? (e->is_dir ? "folder" : "file") : kind_label(k));
        row("Kind", buf);
        row("Type", e->is_link ? "inode/symlink" : mime_of(e->name, e->is_dir));
        if (e->is_dir && !e->is_link) {
            int64_t inside = folders > 0 ? files + folders - 1 : 0;
            fmt_int((uint64_t)inside, a, sizeof a);
            if (!t) snprintf(buf, sizeof buf, "Could not be counted");
            else if (counting) snprintf(buf, sizeof buf, "%s %s so far...", a, plural(inside, "item", "items"));
            else {
                fmt_int((uint64_t)(folders > 0 ? folders - 1 : 0), b, sizeof b);
                char f[64];
                fmt_int((uint64_t)files, f, sizeof f);
                snprintf(buf, sizeof buf, "%s %s (%s %s, %s %s)", a, plural(inside, "item", "items"), b,
                         plural(folders - 1, "folder", "folders"), f, plural(files, "file", "files"));
            }
            row("Contains", buf);
            fmt_size(bytes, buf, sizeof buf);
            if (counting) strncat(buf, " so far...", sizeof buf - strlen(buf) - 1);
            row("Size", buf);
        } else if (!e->is_link) {
            fmt_size(e->size, buf, sizeof buf);
            row("Size", buf);
        }
        row_long("Path", P.path, 1);
        if (P.local && P.info.is_link) {
            char target[sizeof P.info.target + 16];
            snprintf(target, sizeof target, "%s%s", P.info.target, P.info.target_ok ? "" : " (missing)");
            row_long("Links to", target, 2);
        }
    } else {
        int n = snprintf(buf, sizeof buf, "%d items:", P.uris.n);
        const char *sep = " ";
        if (P.folders) n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%d %s", sep, P.folders, plural(P.folders, "folder", "folders")), sep = ", ";
        if (P.files) n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%d %s", sep, P.files, plural(P.files, "file", "files")), sep = ", ";
        if (P.links) n += snprintf(buf + n, sizeof buf - (size_t)n, "%s%d %s", sep, P.links, plural(P.links, "link", "links")), sep = ", ";
        if (P.missing) snprintf(buf + n, sizeof buf - (size_t)n, "%s%d gone", sep, P.missing);
        row("Selected", buf);
        fmt_int((uint64_t)(files + folders), a, sizeof a);
        if (!t) snprintf(buf, sizeof buf, "Could not be counted");
        else if (counting) snprintf(buf, sizeof buf, "%s %s so far...", a, plural(files + folders, "item", "items"));
        else {
            fmt_int((uint64_t)folders, b, sizeof b);
            char f[64];
            fmt_int((uint64_t)files, f, sizeof f);
            snprintf(buf, sizeof buf, "%s %s (%s %s, %s %s)", a, plural(files + folders, "item", "items"), b,
                     plural(folders, "folder", "folders"), f, plural(files, "file", "files"));
        }
        row("In all", buf);
        fmt_size(bytes, buf, sizeof buf);
        if (counting) strncat(buf, " so far...", sizeof buf - strlen(buf) - 1);
        row("Size", buf);
        row_long("In folder", P.path, 1);
    }
    if (unreadable) {
        snprintf(buf, sizeof buf, "%lld %s could not be read", (long long)unreadable, plural(unreadable, "folder", "folders"));
        row_c("Not counted", buf, theme.warning);
    }
    if (counting) ui_redraw_in(0.1);
}

/* a secondary button in the value column that scrolls with the body; 1 when clicked */
static int body_button(const char *id, const char *text, float *right)
{
    float h = DP(ui.touch ? 40 : 32), w = ui_button_width(text);
    Rectangle b = {L.x + L.lw - DP(8), L.y + (row_h() - h) / 2, w, h};
    int state, clicked = ui_clickable(ui_id(id, 0), in_body(b), &state);
    ui_round(b, 8, state == 2 ? theme.pressed : state ? theme.hover : ui_alpha(theme.accent, 0.12f));
    ui_text_box(text, b, 14, FONT_SEMIBOLD, theme.accent, ALIGN_CENTER, 0);
    *right = b.x + b.width;
    return clicked;
}

static void checksums(void)
{
    fr_entry *e = &P.ents[0];
    if (P.uris.n != 1 || !e->name || e->is_dir) return;
    section("Checksums");
    hash_task *t = P.hash;
    float h = row_h(), right;
    if (!t) {
        label("Not computed", h);
        if (body_button("props-hash", "Compute MD5, SHA-1, SHA-256", &right)) hash_start();
        L.y += h;
        return;
    }
    if (!__atomic_load_n(&t->finished, __ATOMIC_ACQUIRE)) {
        uint64_t done = __atomic_load_n(&t->done, __ATOMIC_RELAXED);
        char buf[96], a[32], b[32];
        fr_format_size(done, a, sizeof a);
        fr_format_size(t->total, b, sizeof b);
        snprintf(buf, sizeof buf, "Reading... %s of %s", a, b);
        row("Computing", buf);
        Rectangle bar = {L.x + L.lw, L.y, L.w - L.lw, DP(4)};
        ui_progress(bar, t->total ? (float)((double)done / (double)t->total) : 0, theme.accent);
        L.y += DP(12);
        ui_redraw_in(0.1);
        return;
    }
    if (t->err) {
        row_c("Checksums", t->err == -ECANCELED ? "Stopped" : fr_vfs_error(t->err), theme.danger);
        return;
    }
    static const char *const names[3] = {"MD5", "SHA-1", "SHA-256"};
    for (int i = 0; i < 3; i++) row_long(names[i], t->hex[i], 3 + i);
    label("Compare", h);
    if (body_button("props-compare", "With the clipboard", &right)) compare_clipboard();
    if (P.compared) {
        char buf[64];
        const char *icon = P.compared > 0 ? "circle-check" : P.compared == -1 ? "circle-x" : NULL;
        Color c = P.compared > 0 ? theme.success : P.compared == -1 ? theme.danger : theme.text_faint;
        if (P.compared > 0) snprintf(buf, sizeof buf, "Matches the %s", names[P.compared - 1]);
        else snprintf(buf, sizeof buf, P.compared == -1 ? "No match" : "The clipboard is empty");
        float x = right + DP(12);
        if (icon) ui_icon(icon, (Rectangle){x, L.y, DP(20), h}, 18, c), x += DP(26);
        ui_text_box(buf, (Rectangle){x, L.y, L.x + L.w - x, h}, 14, FONT_SEMIBOLD, c, ALIGN_LEFT, 0);
    }
    L.y += fmaxf(h, DP(ui.touch ? 40 : 32));
}

static void dates(void)
{
    char buf[64];
    section("Dates");
    if (P.info.has_birth) fmt_date(P.info.birth, buf, sizeof buf);
    row_c("Created", P.info.has_birth ? buf : "Not recorded by this filesystem", P.info.has_birth ? theme.text : theme.text_faint);
    fmt_date(P.info.mtime, buf, sizeof buf);
    row("Modified", buf);
    fmt_date(P.info.atime, buf, sizeof buf);
    row("Accessed", buf);
    fmt_date(P.info.ctime, buf, sizeof buf);
    row("Changed", buf);
}

/* a text field in the value column of a row rh high; its TEXT_* */
static int body_field(const char *id, fr_text *t, const char *hint, float w, float rh)
{
    float h = DP(ui.touch ? 40 : 30);
    Rectangle r = {L.x + L.lw - DP(8), L.y + (rh - h) / 2, w, h};
    Rectangle seen = in_body(r);
    if (seen.height < r.height) {
        ui_text_box(t->len ? t->buf : hint, (Rectangle){r.x + DP(8), r.y, w - DP(16), h}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
        return 0;
    }
    return ui_text_field(ui_id(id, 0), t, r, hint);
}

static int submitted; /* Enter in one of the panel's fields */

static void ownership(void)
{
    char buf[128];
    if (!P.editable || !P.all_local || !P.can_set) {
        if (!P.local) return;
        section("Ownership");
        fmt_id(P.info.owner, P.info.uid, buf, sizeof buf);
        row("Owner", buf);
        fmt_id(P.info.group, P.info.gid, buf, sizeof buf);
        row("Group", buf);
        return;
    }
    section("Ownership");
    float h = fmaxf(row_h(), DP(ui.touch ? 44 : 34)), fw = fminf(DP(220), L.w - L.lw);
    label("Owner", h);
    /* giving files away takes root (chown(2)); ferret never asks for it */
    if (geteuid() == 0) submitted |= body_field("props-owner", &P.owner, P.uid < 0 ? "mixed: keep each" : "", fw, h) & TEXT_SUBMIT;
    else ui_text_box(P.uid < 0 ? "Mixed" : P.owner_was, (Rectangle){L.x + L.lw, L.y, L.w - L.lw, h}, 14, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
    L.y += h;
    label("Group", h);
    submitted |= body_field("props-group", &P.group, P.gid < 0 ? "mixed: keep each" : "", fw, h) & TEXT_SUBMIT;
    L.y += h;
    float lh = ui_line_height(13);
    const char *note = geteuid() == 0 ? "A user or group by name or number." : "A group you belong to, by name or number. Only root gives files to another user.";
    int lines = ui_text_wrapped(note, (Rectangle){L.x + L.lw, L.y, L.w - L.lw, 0}, 13, FONT_REGULAR, theme.text_faint, 1);
    L.y += lines * lh + DP(4);
}

static int owner_changed(void) { return strcmp(P.owner.buf, P.owner_was) != 0; }
static int group_changed(void) { return strcmp(P.group.buf, P.group_was) != 0; }

static int dirty(void)
{
    for (int i = 0; i < 9; i++)
        if (P.bit[i] != P.orig[i]) return 1;
    if (P.special >= 0 && (P.special != (int)(P.all_on & 07000) || P.all_on != P.any_on)) return 1;
    if (P.all_local && (owner_changed() || group_changed())) return 1;
    return P.recursive;
}

/* one of n choices as a row of pills; the one picked, else -1 */
static int pills(const char *id, const char *const *labels, int n, int on)
{
    float h = DP(ui.touch ? 36 : 28), x = L.x + L.lw - DP(4), y = L.y + (row_h() - h) / 2;
    int picked = -1;
    for (int i = 0; i < n; i++) {
        float w = ui_text_width(labels[i], 13, FONT_REGULAR) + DP(20);
        Rectangle r = {x, y, w, h};
        int s;
        if (ui_clickable(ui_id(id, i), in_body(r), &s)) picked = i;
        ui_round(r, 14, i == on ? ui_alpha(theme.accent, 0.22f) : s == 2 ? theme.pressed : s ? theme.hover : theme.surface);
        ui_text_box(labels[i], r, 13, i == on ? FONT_SEMIBOLD : FONT_REGULAR, i == on ? theme.accent : theme.text, ALIGN_CENTER, 0);
        x += w + DP(6);
    }
    return picked;
}

static int check_row(const char *id, int on, const char *text)
{
    Rectangle cell = {L.x + L.lw, L.y, DP(24), DP(ui.touch ? 40 : 32)};
    Rectangle hit = {cell.x - DP(4), cell.y, L.w - L.lw + DP(4), cell.height};
    int s, clicked = ui_clickable(ui_id(id, 0), in_body(hit), &s);
    if (s) ui_round(hit, 6, s == 2 ? theme.pressed : theme.hover);
    ui_icon(on ? "square-check" : "square", cell, 20, on ? theme.accent : theme.text_dim);
    ui_text_box(text, (Rectangle){cell.x + DP(32), cell.y, hit.width - DP(36), cell.height}, 14, FONT_REGULAR, theme.text, ALIGN_LEFT, 1);
    L.y += cell.height;
    return clicked;
}

static void permissions(void)
{
    section("Permissions");
    float h = row_h(), cw = fminf((L.w - L.lw) / 3, DP(96));
    if (!P.editable) {
        if (P.local && P.info.is_link && P.info.target_ok) {
            char rwx[10], octal[6], buf[64];
            fr_mode_rwx(P.info.target_mode, rwx);
            fr_mode_octal(P.info.target_mode, octal);
            snprintf(buf, sizeof buf, "%s  %s", octal, rwx);
            row("Target mode", buf);
        }
        row_c("Note", P.links ? "A link has no permissions of its own" : "Could not be looked at", theme.text_faint);
        return;
    }
    int enabled = P.can_set;
    for (int c = 0; c < 3; c++)
        ui_text_box(perms[c], (Rectangle){L.x + L.lw + c * cw, L.y, cw, h}, 13, FONT_SEMIBOLD, theme.text_dim, ALIGN_CENTER, 0);
    L.y += h;
    for (int r = 0; r < 3; r++) {
        float rh = DP(ui.touch ? 40 : 32);
        label(classes[r], rh);
        for (int c = 0; c < 3; c++) {
            int i = r * 3 + c;
            Rectangle cell = {L.x + L.lw + c * cw, L.y, cw, rh};
            if (checkbox(ui_id("props-bit", i), cell, P.bit[i], enabled)) {
                if (P.orig[i] == BIT_MIXED) P.bit[i] = P.bit[i] == BIT_MIXED ? BIT_ON : P.bit[i] == BIT_ON ? BIT_OFF : BIT_MIXED;
                else P.bit[i] = P.bit[i] == BIT_ON ? BIT_OFF : BIT_ON;
                P.error[0] = 0;
                ui.dirty = 1;
            }
        }
        L.y += rh;
    }
    char rwx[10], octal[6], buf[64];
    grid_mode(rwx, octal);
    if (enabled) {
        float h = fmaxf(row_h(), DP(ui.touch ? 44 : 34)), fw = DP(72);
        unsigned oid = ui_id("props-octal", 0);
        if (ui.focus != oid) text_set(&P.octal, octal);
        label("Mode", h);
        int r = body_field("props-octal", &P.octal, "755", fw, h);
        uint32_t m;
        int special;
        if (r & TEXT_CHANGED && fr_mode_parse_octal(P.octal.buf, &m, &special)) {
            for (int i = 0; i < 9; i++) P.bit[i] = m & bit_mask(i) ? BIT_ON : BIT_OFF;
            P.special = special ? (int)(m & 07000) : -1;
            P.error[0] = 0;
            grid_mode(rwx, octal);
        }
        submitted |= r & TEXT_SUBMIT;
        ui_text_box(rwx, (Rectangle){L.x + L.lw - DP(8) + fw + DP(12), L.y, L.w - L.lw - fw, h}, 14, FONT_REGULAR, theme.text, ALIGN_LEFT, 0);
        L.y += h;
    } else {
        snprintf(buf, sizeof buf, "%s  %s", octal, rwx);
        row("Mode", buf);
    }
    special_text(buf, sizeof buf);
    row("Special", buf);
    if (P.folders && enabled) {
        if (check_row("props-recursive", P.recursive,
                      P.folders == 1 && P.uris.n == 1 ? "Apply to everything inside" : "Apply to everything inside the folders"))
            P.recursive = !P.recursive, P.error[0] = 0;
        if (P.recursive) {
            static const char *const only[] = {"Files and folders", "Files only", "Folders only"};
            label("Bits go to", row_h());
            int p = pills("props-only", only, 3, (int)P.only);
            if (p >= 0) P.only = (fr_perm_only)p;
            L.y += row_h() + DP(4);
            if (check_row("props-exec-x", P.exec_x, "Execute only for folders and files already executable (X)")) P.exec_x = !P.exec_x;
            float lh = ui_line_height(13);
            const char *note = P.only == PERM_FILES ? "Folders keep their bits; files get exactly these."
                               : P.only == PERM_FOLDERS ? "Files keep their bits; folders inside also get execute wherever they get read."
                                                        : "Files get exactly these; folders inside also get execute wherever they get read.";
            int lines = ui_text_wrapped(note, (Rectangle){L.x + L.lw, L.y, L.w - L.lw, 0}, 13, FONT_REGULAR, theme.text_faint, 1);
            L.y += lines * lh + DP(4);
        }
    }
    if (!enabled) row_c("Note", "This storage keeps no permissions", theme.text_faint);
    if (P.error[0]) {
        float lh = ui_line_height(14);
        int lines = ui_text_wrapped(P.error, (Rectangle){L.x + L.lw, L.y + DP(4), L.w - L.lw, 0}, 14, FONT_REGULAR, theme.danger, 1);
        L.y += lines * lh + DP(8);
    }
}

static void filesystem(void)
{
    const fr_local_info *in = P.local ? &P.info : &P.folder_info;
    if (!P.has_mount && !P.has_space && !P.local) return;
    char buf[1200], a[32], b[32];
    section("Filesystem");
    if (P.has_mount) {
        row_long("Mount point", in->mount.point, 0);
        row("Type", in->mount.type);
        snprintf(buf, sizeof buf, "%s (%u:%u)", in->mount.source, in->dev_major, in->dev_minor);
        row_long("Device", buf, 0);
    }
    if (P.has_space) {
        fr_format_size(P.space_free, a, sizeof a);
        fr_format_size(P.space_total, b, sizeof b);
        snprintf(buf, sizeof buf, "%s free of %s", a, b);
        row("Space", buf);
    }
    if (P.local) {
        fmt_int(P.info.inode, a, sizeof a);
        row("Inode", a);
        fmt_int(P.info.links, a, sizeof a);
        row("Hard links", a);
    }
}

/* ---- applying ---- */

static void apply(void)
{
    uint32_t set = 0, clear = 0;
    for (int i = 0; i < 9; i++) {
        if (P.bit[i] == BIT_ON) set |= bit_mask(i);
        if (P.bit[i] == BIT_OFF) clear |= bit_mask(i);
    }
    if (P.special >= 0) set |= (uint32_t)P.special & 07000, clear |= ~(uint32_t)P.special & 07000;
    fr_perm_change c = {.set = set, .clear = clear, .recursive = P.recursive && P.folders, .uid = -1, .gid = -1};
    if (c.recursive) c.only = P.only, c.exec_x = P.exec_x;
    uint32_t id;
    if (P.all_local && owner_changed() && P.owner.len) {
        if (fr_owner_lookup(P.owner.buf, 0, &id)) {
            snprintf(P.error, sizeof P.error, "There is no user \"%.200s\" here", P.owner.buf);
            return;
        }
        c.uid = id;
    }
    if (P.all_local && group_changed() && P.group.len) {
        if (fr_owner_lookup(P.group.buf, 1, &id)) {
            snprintf(P.error, sizeof P.error, "There is no group \"%.200s\" here", P.group.buf);
            return;
        }
        c.gid = id;
    }
    fr_strs targets = {0};
    for (int i = 0; i < P.uris.n; i++)
        if (P.ents[i].name && !P.ents[i].is_link) FR_PUSH(targets, fr_strdup(P.uris.v[i]));
    /* as a job, off the window thread for any storage: it shows its progress, says what failed and can be undone */
    if (targets.n) fr_job_submit_perms(&targets, &c);
    fr_strs_free(&targets);
    props_close();
}

/* ---- the panel ---- */

static void apply_button(Rectangle r, int enabled)
{
    if (enabled) {
        if (ui_button(ui_id("props-apply", 0), r, "Apply", 1)) apply();
        return;
    }
    ui_round(r, 8, theme.border);
    ui_text_box("Apply", r, 15, FONT_SEMIBOLD, theme.text_faint, ALIGN_CENTER, 0);
}

int props_draw(void)
{
    if (!P.open) return 0;
    ui_layer(1);
    ui_rect((Rectangle){0, 0, ui.w, ui.h}, theme.scrim);
    Rectangle s = ui.safe;
    float pad = DP(ui.touch ? 20 : 24), w = fminf(s.width - DP(32), DP(560)), head = DP(56), bh = DP(40);
    float foot = bh + 2 * DP(16);
    float max_h = s.height - DP(48), h = fminf(max_h, pad + head + DP(8) + P.content_h + foot);
    Rectangle card = {s.x + (s.width - w) / 2, s.y + (s.height - h) / 2, w, h};
    ui_round(card, 14, theme.surface_hi);
    ui_round_line(card, 14, theme.border);

    float x = card.x + pad, inner = w - 2 * pad;
    const fr_entry *e0 = P.ents && P.ents[0].name ? &P.ents[0] : NULL;
    const char *icon = P.uris.n > 1 ? "file-stack" : e0 ? kind_icon(kind_of(e0->name, e0->is_dir)) : "file";
    Color ic = P.uris.n > 1 || !e0 ? theme.text_dim : kind_color(kind_of(e0->name, e0->is_dir));
    Rectangle ib = {x, card.y + pad, head, head};
    ui_round(ib, 12, ui_alpha(ic, 0.14f));
    if (!(P.uris.n == 1 && e0 && e0->name && !e0->is_link && preview_image_draw(P.uris.v[0], e0, ib) == 1))
        ui_icon(P.uris.n == 1 && e0 && e0->is_link ? "link" : icon, ib, 28, ic);
    char title[300];
    if (P.uris.n == 1) {
        char *name = fr_uri_name(P.uris.v[0]);
        snprintf(title, sizeof title, "%s", *name ? name : "/");
        free(name);
    } else snprintf(title, sizeof title, "%d items", P.uris.n);
    ui_text_box(title, (Rectangle){x + head + DP(16), ib.y, inner - head - DP(16), head}, 18, FONT_SEMIBOLD, theme.text, ALIGN_LEFT, 1);

    float body_y = ib.y + head + DP(8);
    Rectangle body = {card.x, body_y, w, card.y + h - foot - body_y};
    ui_hline(card.x, body.y, w, theme.border);
    ui_hline(card.x, body.y + body.height, w, theme.border);
    ui_scroll(ui_id("props-scroll", 0), &P.scroll, body, P.content_h);
    L.body = body;
    L.x = x, L.w = inner;
    L.lw = fminf(DP(124), L.w * 0.34f);
    L.top = L.y = body.y + DP(8) - P.scroll.offset;
    ui_clip_begin(body);
    submitted = 0;
    if (!P.ents) {
        ui_icon("loader-circle", (Rectangle){L.x, L.y, DP(24), row_h()}, 16, theme.text_dim);
        ui_text_box("Loading...", (Rectangle){L.x + DP(32), L.y, L.w - DP(32), row_h()}, 14, FONT_REGULAR, theme.text_dim, ALIGN_LEFT, 0);
        L.y += row_h();
    } else {
        general();
        checksums();
        if (P.local) dates();
        ownership();
        permissions();
        filesystem();
    }
    ui_clip_end();
    float content = L.y + DP(12) - L.top;
    if (fabsf(content - P.content_h) > 0.5f) P.content_h = content, ui.dirty = 1;

    int open = 1, changed = dirty();
    float by = card.y + h - DP(16) - bh, aw = ui_button_width("Apply"), cw = ui_button_width("Close");
    int can_apply = changed && P.can_set;
    if (P.editable) apply_button((Rectangle){card.x + w - pad - aw, by, aw, bh}, can_apply);
    float cx = card.x + w - pad - (P.editable ? aw + DP(8) : 0) - cw;
    if (ui_button(ui_id("props-close", 0), (Rectangle){cx, by, cw, bh}, "Close", 0)) open = 0;
    if (P.open && can_apply && (submitted || ui_key(KEY_ENTER, 0) || ui_key(KEY_KP_ENTER, 0))) apply();
    if (ui_key(KEY_ESCAPE, 0) || ui_key(KEY_BACK, 0)) open = 0;
    if (ui_live() && ui.pressed && !changed && !CheckCollisionPointRec(ui.mouse, card)) open = 0;
    ui_layer(0);
    if (!P.open) return 0;
    if (!open) props_close();
    return open;
}

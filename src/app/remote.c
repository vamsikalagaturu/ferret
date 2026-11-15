/* Renaming and making files off this device (a server, a phone, Android's document providers) on a thread of their
 * own, so a server that answers slowly holds up no frame; the outcome arrives between frames (jobs/async.h). Inside
 * an archive these run as jobs instead (ops.c). */
#include "app.h"
#include "jobs/async.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int remote_slow(const char *uri) { return uri && !fr_uri_local(uri) && !fr_archive_is_uri(uri) && fr_vfs_backend(uri); }

enum { R_RENAME, R_CREATE, R_FREE_NAME };

typedef struct {
    int kind, folder, rc;
    char *a, *b;      /* rename: from and to; create: the URI; free name: the folder and the name wanted */
    char *result;     /* free name: the URI found */
    unsigned browser; /* the pane that asked */
    remote_done done;
    void (*named)(const char *uri, unsigned ctx);
    unsigned ctx;
} op;

static op *held; /* the last one a sign-in or trust question held up: it runs again once that was answered */

static void op_free(op *o)
{
    if (!o) return;
    free(o->a), free(o->b), free(o->result);
    free(o);
}

static void work(void *p)
{
    op *o = p;
    switch (o->kind) {
    case R_RENAME: o->rc = fr_vfs_rename(o->a, o->b); break;
    case R_CREATE:
        if (o->folder) o->rc = fr_vfs_mkdir(o->a);
        else {
            fr_stream *s = fr_vfs_open_write(o->a, 1, &o->rc);
            if (s) o->rc = s->close(s);
        }
        break;
    case R_FREE_NAME: o->result = fr_uri_free_name(o->a, o->b, o->folder); break;
    }
}

/* the pane that asked, reloaded on the new name; nothing when its tab closed meanwhile */
static void land(const op *o)
{
    fr_browser *b = browser_by_id(o->browser), *was = br_cur;
    if (!b) return;
    unsigned salt = ui.id_salt;
    browser_enter(b);
    browser_name_row(o->kind == R_RENAME ? o->a : NULL, o->kind == R_RENAME ? o->b : o->a, o->folder);
    free(br.focus_name);
    br.focus_name = fr_uri_name(o->kind == R_RENAME ? o->b : o->a);
    browser_reload();
    br_cur = was;
    ui.id_salt = salt;
}

static void submit(op *o);

static void retry(void)
{
    op *o = held;
    held = NULL;
    if (o) submit(o);
}

static void done(void *p)
{
    op *o = p;
    if (o->kind == R_FREE_NAME) {
        o->named(o->result, o->ctx);
        op_free(o);
        return;
    }
    int keep = 0;
    if (!o->rc) {
        if (o->kind == R_RENAME) undo_record_rename(o->a, o->b);
        else undo_record_created(o->a, o->folder);
        land(o);
    } else if (net_ask(o->a, o->rc, retry)) {
        op_free(held);
        held = o, keep = 1;
    } else {
        char *name = fr_uri_name(o->kind == R_RENAME ? o->b : o->a);
        if (o->rc == -EEXIST) ui_toast("\"%s\" already exists here", name);
        else if (o->kind == R_RENAME) ui_toast("Could not rename: %s", fr_vfs_error(o->rc));
        else ui_toast("Could not make the %s: %s", o->folder ? "folder" : "file", fr_vfs_error(o->rc));
        free(name);
    }
    o->done(o->rc, o->ctx);
    if (!keep) op_free(o);
}

static void submit(op *o)
{
    o->rc = 0;
    net_prepare(o->a);
    fr_async_run(work, done, o);
}

static op *op_new(int kind, const char *a, const char *b, unsigned ctx)
{
    op *o = fr_realloc(NULL, sizeof *o);
    *o = (op){.kind = kind, .a = fr_strdup(a), .b = b ? fr_strdup(b) : NULL, .browser = br.id, .ctx = ctx};
    return o;
}

void remote_rename(const char *from, const char *to, remote_done d, unsigned ctx)
{
    op *o = op_new(R_RENAME, from, to, ctx);
    o->done = d;
    submit(o);
}

void remote_create(const char *uri, int folder, remote_done d, unsigned ctx)
{
    op *o = op_new(R_CREATE, uri, NULL, ctx);
    o->folder = folder, o->done = d;
    submit(o);
}

void remote_free_name(const char *dir, const char *name, int is_dir, void (*named)(const char *uri, unsigned ctx), unsigned ctx)
{
    op *o = op_new(R_FREE_NAME, dir, name, ctx);
    o->folder = is_dir, o->named = named;
    submit(o);
}

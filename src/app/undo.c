#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fr_history hist;
static struct {
    int id, redo; /* the undo or redo job running, 0 none */
    char *label;
} running;

/* inside an archive a change rewrites the archive as a job of its own, which steps do not; those are left out */
static int in_archive(const fr_step *s) { return fr_archive_is_uri(s->a) || (s->b && fr_archive_is_uri(s->b)); }

static void follow_renames(const fr_steps *steps)
{
    for (int i = 0; i < steps->n; i++)
        if (steps->v[i].kind == STEP_RENAME) browser_results_renamed(steps->v[i].a, steps->v[i].b);
}

void undo_record(const char *label, fr_steps *steps)
{
    int kept = 0;
    for (int i = 0; i < steps->n; i++) {
        if (in_archive(&steps->v[i])) free(steps->v[i].a), free(steps->v[i].b);
        else steps->v[kept++] = steps->v[i];
    }
    steps->n = kept;
    follow_renames(steps);
    if (!steps->n) {
        fr_steps_free(steps);
        return;
    }
    fr_history_push(&hist, (fr_op){fr_strdup(label), *steps});
    *steps = (fr_steps){0};
}

void undo_record_rename(const char *from, const char *to)
{
    char *a = fr_uri_name(from), *b = fr_uri_name(to), *label = fr_format("renamed %s -> %s", a, b);
    fr_steps steps = {0};
    fr_steps_add(&steps, STEP_RENAME, from, to);
    undo_record(label, &steps);
    free(a), free(b), free(label);
}

void undo_record_created(const char *uri, int folder)
{
    char *name = fr_uri_name(uri), *label = fr_format("made %s%s", folder ? "the folder " : "", name);
    fr_steps steps = {0};
    fr_steps_add(&steps, STEP_CREATED, uri, NULL);
    steps.v[0].is_dir = folder;
    undo_record(label, &steps);
    free(name), free(label);
}

/* the items a rename plan renamed: its steps to a temporary name do not count */
static int renamed(const fr_steps *log)
{
    int n = 0;
    for (int i = 0; i < log->n; i++) {
        char *name = fr_uri_name(log->v[i].b ? log->v[i].b : log->v[i].a);
        n += strncmp(name, ".ferret-rename-", 15) != 0;
        free(name);
    }
    return n;
}

/* "copied a.txt to Documents", "moved 3 items to the trash" */
static char *label_for(const fr_job_status *st, const fr_steps *log)
{
    char *first = log->n ? fr_uri_name(log->v[0].kind == STEP_COPIED ? log->v[0].b : log->v[0].a) : fr_strdup("");
    char *what = log->n == 1 ? fr_format("%s", first) : fr_format("%d items", log->n), *label = NULL;
    switch (st->kind) {
    case JOB_COPY: label = fr_format("copied %s to %s", what, st->dest_name); break;
    case JOB_MOVE: label = fr_format("moved %s to %s", what, st->dest_name); break;
    case JOB_TRASH: label = fr_format("moved %s to the trash", what); break;
    case JOB_CHMOD: label = fr_format("changed the permissions of %d %s", st->items, st->items == 1 ? "item" : "items"); break;
    case JOB_RENAME:
        if (log->n) {
            char *to = fr_uri_name(log->v[0].b);
            label = fr_format("renamed %s -> %s", first, to);
            free(to);
        }
        break;
    case JOB_CREATE: label = fr_format("made %s", first); break;
    case JOB_STEPS: label = fr_format("renamed %d %s", renamed(log), renamed(log) == 1 ? "item" : "items"); break;
    default: break;
    }
    free(first), free(what);
    return label;
}

int undo_job_done(const fr_job_status *st)
{
    fr_steps log = {0};
    if (fr_job_take_log(st->id, &log)) return 0;
    if (running.id && st->id == running.id) {
        follow_renames(&log);
        int redo = running.redo, ok = !st->failed && st->state == JOB_DONE;
        char *label = running.label;
        running.id = 0, running.label = NULL;
        if (ok) ui_toast("%s: %s", redo ? "Redid" : "Undid", label);
        if (log.n) fr_history_put(&hist, !redo, (fr_op){label, log});
        else free(label), fr_steps_free(&log);
        return ok;
    }
    int toasted = 0;
    if (st->kind == JOB_STEPS && !st->failed && st->state == JOB_DONE) {
        int n = renamed(&log);
        ui_toast("Renamed %d %s", n, n == 1 ? "item" : "items");
        toasted = 1;
    }
    char *label = label_for(st, &log);
    if (label) undo_record(label, &log);
    else fr_steps_free(&log);
    free(label);
    return toasted;
}

int undo_can(int redo) { return redo ? hist.redo.n > 0 : hist.undo.n > 0; }

void undo_run(int redo)
{
    if (running.id) {
        ui_toast("The last %s is still running", running.redo ? "redo" : "undo");
        return;
    }
    fr_op op;
    if (fr_history_pop(&hist, redo, &op)) {
        ui_toast(redo ? "Nothing to redo" : "Nothing to undo");
        return;
    }
    fr_steps inv = fr_steps_inverse(&op.steps);
    for (int i = 0; i < inv.n; i++)
        if (inv.v[i].kind == STEP_TRASH && !inv.v[i].may_remove && !fr_vfs_can_trash(inv.v[i].a)) {
            ui_toast("Cannot %s \"%s\": there is no trash here to put the copies in", redo ? "redo" : "undo", op.label);
            fr_steps_free(&inv);
            fr_history_put(&hist, redo, op);
            return;
        }
    running.id = fr_job_submit_steps(&inv, redo ? "Redoing" : "Undoing");
    running.redo = redo;
    running.label = op.label;
    op.label = NULL;
    fr_steps_free(&inv);
    fr_op_free(&op);
}

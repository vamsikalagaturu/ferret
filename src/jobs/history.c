#include "history.h"
#include <stdlib.h>
#include <string.h>

void fr_steps_add(fr_steps *s, fr_step_kind kind, const char *a, const char *b)
{
    fr_step st = {.kind = kind, .a = fr_strdup(a), .b = b ? fr_strdup(b) : NULL};
    FR_PUSH(*s, st);
}

void fr_steps_add_chmod(fr_steps *s, const char *a, uint32_t old_mode, uint32_t mode)
{
    fr_step st = {.kind = STEP_CHMOD, .a = fr_strdup(a), .mode = mode, .old_mode = old_mode};
    FR_PUSH(*s, st);
}

void fr_steps_free(fr_steps *s)
{
    for (int i = 0; i < s->n; i++) free(s->v[i].a), free(s->v[i].b);
    free(s->v);
    *s = (fr_steps){0};
}

static fr_step step_dup(const fr_step *st)
{
    fr_step c = *st;
    c.a = fr_strdup(st->a);
    c.b = st->b ? fr_strdup(st->b) : NULL;
    return c;
}

fr_steps fr_steps_copy(const fr_steps *s)
{
    fr_steps r = {0};
    for (int i = 0; i < s->n; i++) FR_PUSH(r, step_dup(&s->v[i]));
    return r;
}

fr_steps fr_steps_inverse(const fr_steps *s)
{
    fr_steps r = {0};
    for (int i = s->n - 1; i >= 0; i--) {
        const fr_step *st = &s->v[i];
        fr_step inv = {.is_dir = st->is_dir};
        switch (st->kind) {
        case STEP_RENAME:
            inv.kind = STEP_RENAME, inv.a = fr_strdup(st->b), inv.b = fr_strdup(st->a);
            break;
        case STEP_COPIED: inv.kind = STEP_TRASH, inv.a = fr_strdup(st->b); break;
        case STEP_CREATED: inv.kind = STEP_TRASH, inv.a = fr_strdup(st->a), inv.may_remove = 1; break;
        case STEP_REMOVED: inv.kind = STEP_CREATED, inv.a = fr_strdup(st->a); break;
        case STEP_TRASH: inv.kind = STEP_UNTRASH, inv.a = fr_strdup(st->a); break;
        case STEP_UNTRASH: inv.kind = STEP_TRASH, inv.a = fr_strdup(st->a); break;
        case STEP_CHMOD:
            inv.kind = STEP_CHMOD, inv.a = fr_strdup(st->a), inv.mode = st->old_mode, inv.old_mode = st->mode;
            break;
        }
        FR_PUSH(r, inv);
    }
    return r;
}

void fr_op_free(fr_op *op)
{
    free(op->label);
    fr_steps_free(&op->steps);
    *op = (fr_op){0};
}

#define SIDE(h, redo) ((redo) ? &(h)->redo : &(h)->undo)

void fr_history_put(fr_history *h, int redo, fr_op op)
{
    fr_ops *side = SIDE(h, redo);
    if (side->n == FR_HISTORY_MAX) {
        fr_op_free(&side->v[0]);
        memmove(side->v, side->v + 1, sizeof *side->v * (size_t)(side->n - 1));
        side->n--;
    }
    FR_PUSH(*side, op);
}

void fr_history_push(fr_history *h, fr_op op)
{
    for (int i = 0; i < h->redo.n; i++) fr_op_free(&h->redo.v[i]);
    h->redo.n = 0;
    fr_history_put(h, 0, op);
}

int fr_history_pop(fr_history *h, int redo, fr_op *out)
{
    fr_ops *side = SIDE(h, redo);
    if (!side->n) return -1;
    *out = side->v[--side->n];
    return 0;
}

void fr_history_clear(fr_history *h)
{
    for (int i = 0; i < h->undo.n; i++) fr_op_free(&h->undo.v[i]);
    for (int i = 0; i < h->redo.n; i++) fr_op_free(&h->redo.v[i]);
    free(h->undo.v), free(h->redo.v);
    *h = (fr_history){0};
}

#ifndef FR_HISTORY_H
#define FR_HISTORY_H
/* What a finished operation did, as steps that can be played backwards: undo runs a record's inverse as a job,
 * and what that job did becomes the record redo plays backwards in turn. Nothing outlives the process. */
#include "core/util.h"
#include <stdint.h>

typedef enum {
    STEP_RENAME,  /* a moved to b (a rename, or a move to another folder) */
    STEP_COPIED,  /* b was made as a copy of a */
    STEP_CREATED, /* a was made, an empty file or folder */
    STEP_REMOVED, /* a, empty, was removed */
    STEP_TRASH,   /* a went to the trash */
    STEP_UNTRASH, /* a came back from the trash */
    STEP_CHMOD,   /* a's permission bits went from old_mode to mode */
    STEP_REPLACED,   /* what was at b went to the trash, and a was copied there */
    STEP_UNREPLACED, /* b, still the copy of a, was removed and what the trash had there came back */
    STEP_LINKED,     /* a was made, a symbolic link to b (as written in it) */
    STEP_UNLINKED,   /* a, a symbolic link to b, was removed */
    STEP_CHOWN,      /* a's owner and group went from old_uid:old_gid to uid:gid */
} fr_step_kind;

typedef struct {
    fr_step_kind kind;
    char *a, *b;
    int is_dir;
    int may_remove; /* STEP_TRASH: where there is no trash, removing a when it is empty will do */
    uint32_t mode, old_mode;
    uint32_t uid, gid, old_uid, old_gid;
} fr_step;

typedef struct {
    fr_step *v;
    int n, cap;
} fr_steps;

void fr_steps_add(fr_steps *s, fr_step_kind kind, const char *a, const char *b);
void fr_steps_add_chmod(fr_steps *s, const char *a, uint32_t old_mode, uint32_t mode);
void fr_steps_add_chown(fr_steps *s, const char *a, uint32_t old_uid, uint32_t old_gid, uint32_t uid, uint32_t gid);
void fr_steps_free(fr_steps *s);
fr_steps fr_steps_copy(const fr_steps *s);
/* the steps that take back s: reversed, each turned around (a copy goes to the trash, a creation too) */
fr_steps fr_steps_inverse(const fr_steps *s);

typedef struct {
    char *label; /* "renamed a.txt -> b.txt", for "Undid: ..." */
    fr_steps steps;
} fr_op;

void fr_op_free(fr_op *op);

#define FR_HISTORY_MAX 100
typedef struct {
    fr_op *v;
    int n, cap;
} fr_ops;
typedef struct {
    fr_ops undo, redo;
} fr_history;

/* a new operation, taken: what could be redone is forgotten, the oldest goes past FR_HISTORY_MAX */
void fr_history_push(fr_history *h, fr_op op);
/* the latest to undo (redo 0) or redo (redo 1), taken off: 0, or -1 when there is none */
int fr_history_pop(fr_history *h, int redo, fr_op *out);
/* after an undo ran: what it did, onto the redo side (redo 1), or after a redo, onto the undo side (redo 0) */
void fr_history_put(fr_history *h, int redo, fr_op op);
void fr_history_clear(fr_history *h);
#endif

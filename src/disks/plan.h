#ifndef FR_DISKS_PLAN_H
#define FR_DISKS_PLAN_H
/* Staged changes: delete, create, format, label, resize and image writes are queued, the resulting layout is
 * projected onto the current one, and nothing touches a disk until the queue's steps run in order (the platform runs
 * them; see plat_dm_apply in platform.h). Errors are returned as text, NULL when there is none. */
#include "disks.h"

enum { OP_DELETE = 1, OP_CREATE, OP_FORMAT, OP_LABEL, OP_RESIZE, OP_IMAGE };

typedef struct {
    int type;
    char *disk, *target; /* a disk's path; a segment key ("new:<n>" for a create), the disk for an image */
    char *fs_type, *label, *file;
    uint64_t offset, size, from;
    char *text; /* what it does, for the queue */
} dk_op;

typedef struct {
    dk_op *v;
    int n, cap;
} dk_ops;

typedef struct {
    dk_disks base;  /* as the disks are */
    dk_disks disks; /* as they will be */
    dk_ops ops;
    char error[512]; /* why steps were dropped or the run stopped, "" none */
    int running, done, total, next_id;
    char why[256]; /* the last refusal's text */
} dk_plan;

void dk_plan_free(dk_plan *p);
void dk_plan_set_base(dk_plan *p, dk_disks *base); /* takes base; drops queued steps the new layout no longer allows */

const char *dk_plan_stage_delete(dk_plan *p, const char *disk, const char *key);
const char *dk_plan_stage_create(dk_plan *p, const char *disk, uint64_t offset, uint64_t size, const char *fs, const char *label);
const char *dk_plan_stage_format(dk_plan *p, const char *disk, const char *key, const char *fs, const char *label);
const char *dk_plan_stage_label(dk_plan *p, const char *disk, const char *key, const char *label);
const char *dk_plan_stage_resize(dk_plan *p, const char *disk, const char *key, uint64_t size);
/* a whole-disk raw write of a bootable image (dk_image_check), removable or USB disks only */
const char *dk_plan_stage_image(dk_plan *p, const char *disk, const char *file);

/* why action (an OP_*) is not possible on a segment of the projected layout now */
const char *dk_plan_blocked(dk_plan *p, const char *disk, const char *key, int action);
/* the sizes a segment can be resized to in the projected layout; 0 when there is no such segment */
int dk_plan_limits(const dk_plan *p, const char *disk, const char *key, uint64_t *min, uint64_t *max);

void dk_plan_remove(dk_plan *p, int index);
void dk_plan_clear(dk_plan *p);
/* the disks' device names (sda nvme0n1) the user types to unlock Apply, space-separated */
void dk_plan_confirm_words(const dk_plan *p, char *buf, size_t len);

/* ---- running the queue ---- */
enum { CALL_DELETE = 1, CALL_CREATE, CALL_CREATE_FORMAT, CALL_FORMAT, CALL_SET_LABEL, CALL_PART_RESIZE, CALL_FS_RESIZE, CALL_IMAGE };

/* one UDisks2 method call; CALL_IMAGE is Block.OpenDevice("rw") on object, the write and read-back, Block.Rescan */
typedef struct {
    int method;
    char *object;
    uint64_t offset, size;
    char *ptype, *fs, *label, *file;
} dk_call;

typedef struct {
    dk_call *v;
    int n, cap;
} dk_step;

typedef struct {
    dk_step *v;
    int n, cap;
} dk_steps;

const char *dk_call_method(int method); /* "Delete", "OpenDevice", ...: what dk_refusal judges */
/* the queue as calls, one step per queued change, and the plan marked running; 0 when there is nothing to run */
int dk_plan_begin(dk_plan *p, dk_steps *out);
void dk_plan_step_done(dk_plan *p);
/* error NULL: every step ran and the queue empties; else the steps from the failed one on stay queued */
void dk_plan_finish(dk_plan *p, const char *error);
void dk_steps_free(dk_steps *s);
#endif

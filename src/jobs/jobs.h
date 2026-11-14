#ifndef FR_JOBS_H
#define FR_JOBS_H
/* Copy, move, delete and trash run one at a time on a worker thread. The window polls their state; a name that
 * is taken in the destination pauses the job until the window answers. */
#include "core/util.h"
#include <stdint.h>

typedef enum { JOB_COPY, JOB_MOVE, JOB_DELETE, JOB_TRASH } fr_job_kind;
typedef enum { JOB_QUEUED, JOB_RUNNING, JOB_ASKING, JOB_DONE, JOB_CANCELLED } fr_job_state;
typedef enum { CONFLICT_ASK, CONFLICT_SKIP, CONFLICT_REPLACE, CONFLICT_KEEP_BOTH } fr_conflict;

typedef struct {
    int id;
    fr_job_kind kind;
    fr_job_state state;
    int items;                  /* what was chosen */
    uint64_t bytes_done, bytes_total;
    int files_done, files_total;
    char current[256];          /* the name being worked on */
    char dest_name[256];        /* the destination folder's name */
    int failed;                 /* items that could not be done */
    char error[256];            /* the first failure, "name: reason" */
    char conflict[256];         /* JOB_ASKING: the taken name */
    int conflict_dir;           /* ... a folder on both sides */
} fr_job_status;

/* wake: called from the worker when something changed (the window sleeps between events) */
void fr_jobs_start(void (*wake)(void));
void fr_jobs_stop(void);

/* sources are URIs (copied); dest_dir is ignored for delete and trash. The job's id. */
int fr_job_submit(fr_job_kind kind, const fr_strs *sources, const char *dest_dir);
/* the jobs not yet dismissed, oldest first; at most max */
int fr_jobs_list(fr_job_status *out, int max);
void fr_job_answer(int id, fr_conflict answer, int for_all);
void fr_job_cancel(int id);
void fr_job_dismiss(int id); /* forget a finished job */
/* grows each time a job finishes, so the window knows to reload what it shows */
int fr_jobs_finished(void);
/* waits until no job is queued or running (tests) */
void fr_jobs_wait_idle(void);
#endif

#ifndef FR_JOBS_H
#define FR_JOBS_H
/* Copy, move, delete, trash and the archive work run one at a time on a worker thread. The window polls their state; a name that
 * is taken in the destination pauses the job until the window answers. */
#include "core/util.h"
#include "jobs/history.h"
#include "jobs/perms.h"
#include <stdint.h>

typedef enum { JOB_COPY, JOB_MOVE, JOB_DELETE, JOB_TRASH, JOB_CHMOD, JOB_EXTRACT, JOB_COMPRESS, JOB_RENAME, JOB_CREATE, JOB_STEPS, JOB_FETCH,
               JOB_SYNC, JOB_LINK } fr_job_kind;
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
    char what[64];              /* JOB_STEPS: its verb, "Renaming", "Undoing" */
} fr_job_status;

/* wake: called from the worker when something changed (the window sleeps between events) */
void fr_jobs_start(void (*wake)(void));
void fr_jobs_stop(void);

/* sources are URIs (copied); dest_dir is ignored for delete and trash. The job's id. */
int fr_job_submit(fr_job_kind kind, const fr_strs *sources, const char *dest_dir);
/* JOB_CHMOD: the permission bits of sources and everything under them, as fr_mode_edit() makes them; symlinks
 * are left alone */
int fr_job_submit_chmod(const fr_strs *sources, uint32_t set, uint32_t clear);
/* JOB_CHMOD as c says: the bits (to what is inside folders too when recursive), the owner and group (local files);
 * symlinks are left alone */
int fr_job_submit_perms(const fr_strs *sources, const fr_perm_change *c);
/* JOB_SYNC: a plan of fr_sync_plan()'s (jobs/compare.h). A copy that finds its name taken asks; what is replaced
 * and what a mirror removes go to the trash, and where there is none they are deleted. */
int fr_job_submit_sync(const fr_steps *plan, const char *dest_dir);
/* JOB_LINK (local files): a symbolic link in dest_dir to each source, written relative to dest_dir or absolute */
int fr_job_submit_link(const fr_strs *sources, const char *dest_dir, int relative);
/* JOB_STEPS: the steps in order, going on past one that fails; a rename never replaces, and one to another
 * device copies and then deletes */
int fr_job_submit_steps(const fr_steps *steps, const char *what);
/* a finished job's record of what it did, as steps undo can turn around (copies, moves, trashing, permissions,
 * owners, syncs, links and steps; a permanent delete, a file replaced without a trash to put it in and a merged
 * folder leave none); 0, or -1 while it runs */
int fr_job_take_log(int id, fr_steps *out);
/* Copying, moving and deleting into, out of or inside an archive (vfs/vfs_archive.h) take one pass over it each;
 * a change inside rewrites it whole, and lands all at once or not at all. */
/* JOB_EXTRACT: what each archive holds into dest_dir, or (into_folder) into a new folder there named after it */
int fr_job_submit_extract(const fr_strs *archives, const char *dest_dir, int into_folder);
/* JOB_COMPRESS: sources into a new archive at archive_uri, its kind by its name */
int fr_job_submit_compress(const fr_strs *sources, const char *archive_uri);
/* JOB_RENAME, JOB_CREATE (a folder, else an empty file): for where these take long, inside an archive */
int fr_job_submit_rename(const char *from, const char *to);
int fr_job_submit_create(const char *uri, int folder);
/* JOB_FETCH: a read-only copy of the file uri at copy (a file:// URI, core/opencache.h) for an app to open; one
 * there already with the source's size and mtime is kept. Leaves nothing to undo. */
int fr_job_submit_fetch(const char *uri, const char *copy);
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

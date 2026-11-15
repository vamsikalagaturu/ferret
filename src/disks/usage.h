#ifndef FR_DISKS_USAGE_H
#define FR_DISKS_USAGE_H
/* "Look inside": what takes the space on one volume. Scans its tree on a worker thread (one filesystem only, hard
 * links counted once, sizes as allocated blocks like du) and shows one folder level at a time. Results are kept per
 * volume for the session, so going back and in again is instant; leaving the view does not stop a running scan.
 * Only one scan runs: starting another drops the running one. POSIX only (openat, fstatat, fdopendir, statvfs). */
#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum { SCAN_MOUNTING = 1, SCAN_SCANNING, SCAN_DONE, SCAN_ERROR };

typedef struct dk_node dk_node;
struct dk_node {
    char *name;
    int dir, denied;
    uint64_t size, files, no_access;
    uint64_t other_size, other_files; /* the files past the largest few, summed */
    dk_node **kids;
    int nkids, cap;
};

typedef struct {
    unsigned id; /* new for every scan, so a view can tell its folder path went stale */
    char *key, *title, *mount_point;
    int status;
    char error[256];
    time_t when;
    uint64_t used;           /* by the filesystem, statvfs */
    uint64_t scanned, files; /* so far; atomic */
    char current[256];       /* the top-level folder being counted; under dk_usage_lock */
    dk_node *root;           /* its kids grow under dk_usage_lock while scanning; finished subtrees stay as they are */
} dk_scan;

dk_scan *dk_usage_find(const char *key);
/* a volume that has to be mounted first: dk_usage_start or dk_usage_fail follows */
dk_scan *dk_usage_mounting(const char *key, const char *title);
dk_scan *dk_usage_start(const char *key, const char *title, const char *mount_point); /* a fresh scan, replacing the cached one */
void dk_usage_fail(const char *key, const char *error);
const char *dk_usage_active(void); /* the key mounting or being scanned, NULL none */
/* drops the active scan and its result; the key it had (malloc'd), NULL none */
char *dk_usage_cancel(void);
/* the key whose scan just ended (malloc'd), else NULL; call once a frame */
char *dk_usage_poll(void);
void dk_usage_forget(void); /* every result, e.g. after the queue changed partitions */
void dk_usage_lock(void);
void dk_usage_unlock(void);

enum { ROW_DIR, ROW_FILE, ROW_DENIED, ROW_OTHER, ROW_COUNTING, ROW_UNACCOUNTED };

typedef struct {
    char name[256];
    int kind, index; /* index: the node in its folder's kids, for entering; -1 */
    uint64_t size, files;
    double share; /* of the folder, of the volume's used space at the top */
} dk_row;

typedef struct {
    dk_row *v;
    int n, cap;
    uint64_t used, scanned, unaccounted, files, here, here_files;
} dk_rows;

/* the rows of folder at (the root when NULL), biggest first, up to 200 plus a row of the rest; locks itself */
void dk_usage_rows(const dk_scan *s, const dk_node *at, dk_rows *out);
#endif

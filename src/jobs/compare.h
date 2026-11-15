#ifndef FR_COMPARE_H
#define FR_COMPARE_H
/* Two folders side by side: what is only on one side, what differs and what is alike, and the copies that bring one
 * side up to date with the other (a sync), as steps a JOB_SYNC runs. */
#include "jobs/history.h"
#include "vfs/vfs.h"

typedef enum {
    CMP_NONE,      /* this side has no such item */
    CMP_ONLY,      /* only this side has it */
    CMP_NEWER,     /* both have it; this side's was modified later */
    CMP_OLDER,     /* ... earlier */
    CMP_DIFFERENT, /* both, neither newer: another size or content, or a file against a folder */
    CMP_SAME,      /* both, alike (a folder: everything inside too) */
    CMP_FOLDER,    /* a folder on both sides, not looked into */
    CMP_INSIDE,    /* a folder on both sides with differences inside */
} fr_cmp_state;

typedef struct {
    int recursive; /* folders on both sides are compared inside */
    int content;   /* files of the same size are read and compared byte for byte */
    int tolerance; /* seconds two modification times may be apart and count as one */
} fr_cmp_options;

/* FAT keeps modification times to 2 seconds (Microsoft's FAT32 File System Specification, "Directory Entry"),
 * which copies to and from USB sticks and SD cards round them to */
#define FR_CMP_TOLERANCE 2

typedef struct {
    char *path;                /* below the two folders, "docs/a.txt" */
    fr_cmp_state left, right;  /* how each side's item compares with the other's */
    fr_entry l, r;             /* name NULL where that side has none */
} fr_cmp_item;

typedef struct {
    fr_cmp_item *v;
    int n, cap;
    int top; /* v[0..top) are those at the top, in strcmp() order of path */
} fr_cmp_items;

/* a, this side's, against b (NULL: the other side has none); same_content is 1 or 0 when the files were compared
 * byte for byte, -1 when not */
fr_cmp_state fr_cmp_classify(const fr_entry *a, const fr_entry *b, int tolerance, int same_content);
/* left and right compared into out: every item the two hold at the top, and below folders on both sides (recursive)
 * those that differ; *scanned counts the items looked at (another thread may read it), *cancel stops it early.
 * 0, or the error listing left or right. */
int fr_cmp_folders(const char *left, const char *right, const fr_cmp_options *o, fr_cmp_items *out, int64_t *scanned,
                   const int *cancel);
void fr_cmp_items_free(fr_cmp_items *items);
/* the item named name at the top, NULL none */
const fr_cmp_item *fr_cmp_find(const fr_cmp_items *items, const char *name);

typedef struct {
    int copy, replace, remove; /* the steps of each kind */
    int skipped;               /* differ, but the side copied from is not newer (a mirror replaces them) */
    int extra;                 /* only on the side copied to (a mirror removes them) */
    uint64_t bytes;            /* what is copied, folders not counted inside */
} fr_sync_counts;

/* the steps that copy from the left folder to the right one (to_right) or back: what is missing there
 * (STEP_COPIED a -> b) and what is newer here (STEP_REPLACED); a mirror also replaces what differs otherwise and
 * moves what only the other side has to the trash (STEP_TRASH) */
fr_steps fr_sync_plan(const fr_cmp_items *items, const char *left, const char *right, int to_right, int mirror,
                      fr_sync_counts *counts);
#endif

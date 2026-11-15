#ifndef FR_GITSTATUS_H
#define FR_GITSTATUS_H
/* What git says about the items of a folder inside a work tree, from `git status --porcelain=v2 -z
 * --ignored=matching` (git-status(1), "Porcelain Format Version 2"). Only that read-only command is run, with
 * --no-optional-locks so it does not even refresh the index. */
#include "core/util.h"

enum {
    GIT_MODIFIED = 1,  /* changed in the work tree, against the index (deleted too) */
    GIT_STAGED = 2,    /* changed in the index, against HEAD */
    GIT_UNTRACKED = 4,
    GIT_IGNORED = 8,
    GIT_CONFLICT = 16, /* unmerged */
};

typedef struct {
    char *name; /* an item of the folder */
    int flags;  /* GIT_*; a folder's are those of everything below it, but GIT_IGNORED only for itself */
} fr_git_mark;

typedef struct {
    fr_git_mark *v;
    int n, cap; /* by strcmp() order of name */
} fr_git_marks;

/* the porcelain v2 -z output (paths from the work tree's top) as marks for the folder prefix ("" the top, else
 * "a/b" without slashes around it); 0, or -1 when it is not that format */
int fr_git_parse(const char *out, size_t len, const char *prefix, fr_git_marks *marks);
int fr_git_flags(const fr_git_marks *marks, const char *name); /* 0 when it has none */
void fr_git_marks_free(fr_git_marks *marks);

/* the top of the work tree holding the folder dir (a path): the nearest folder up from it with a .git; NULL none */
char *fr_git_top(const char *dir);
/* git's marks for the items of the local folder dir, waiting at most timeout_ms for git: 0; -ENOENT when dir is in
 * no work tree or git is not installed, -ETIMEDOUT, or -EIO when git failed */
int fr_git_status(const char *dir, int timeout_ms, fr_git_marks *marks);
int fr_git_available(void); /* a git on $PATH */
#endif

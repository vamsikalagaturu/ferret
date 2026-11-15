#define _GNU_SOURCE
#include "gitstatus.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static void add_mark(fr_git_marks *m, const char *name, size_t len, int flags)
{
    fr_git_mark k = {fr_strndup(name, len), flags};
    FR_PUSH(*m, k);
}

static int by_name(const void *a, const void *b) { return strcmp(((const fr_git_mark *)a)->name, ((const fr_git_mark *)b)->name); }

/* path (from the top) as what it means for the items of prefix */
static void mark_path(fr_git_marks *m, const char *path, const char *prefix, int flags)
{
    size_t pl = strlen(prefix), len = strlen(path);
    const char *rest = path;
    if (pl) {
        /* a folder above prefix, untracked or ignored as a whole, takes in everything shown */
        if (len && path[len - 1] == '/' && len - 1 <= pl && !strncmp(path, prefix, len - 1) && (len - 1 == pl || prefix[len - 1] == '/')) {
            add_mark(m, "", 0, flags);
            return;
        }
        if (strncmp(path, prefix, pl) || path[pl] != '/') return;
        rest = path + pl + 1;
    }
    size_t name_len = strcspn(rest, "/");
    if (!name_len) return;
    int deeper = rest[name_len] == '/' && rest[name_len + 1];
    add_mark(m, rest, name_len, deeper ? flags & ~GIT_IGNORED : flags);
}

/* the path after skip space-separated fields */
static const char *after_fields(const char *rec, int skip)
{
    const char *p = rec;
    for (int i = 0; i < skip; i++) {
        p = strchr(p, ' ');
        if (!p) return NULL;
        p++;
    }
    return p;
}

static int xy_flags(const char *rec)
{
    const char *xy = rec + 2;
    if (strlen(rec) < 4) return 0;
    return (xy[0] != '.' ? GIT_STAGED : 0) | (xy[1] != '.' ? GIT_MODIFIED : 0);
}

int fr_git_parse(const char *out, size_t len, const char *prefix, fr_git_marks *marks)
{
    fr_git_marks m = {0};
    int bad = 0;
    for (size_t i = 0; i < len && !bad;) {
        const char *rec = out + i;
        size_t n = strnlen(rec, len - i);
        if (n == len - i) {
            bad = 1;
            break;
        }
        i += n + 1;
        if (n < 2 || rec[1] != ' ') {
            bad = n != 0;
            continue;
        }
        const char *path = NULL;
        int flags = 0;
        switch (rec[0]) {
        case '#': continue;
        case '1': path = after_fields(rec, 8), flags = xy_flags(rec); break;
        case '2':
            path = after_fields(rec, 9), flags = xy_flags(rec);
            i += strnlen(out + i, len - i) + 1; /* the path it had before, which is not here any more */
            break;
        case 'u': path = after_fields(rec, 10), flags = GIT_CONFLICT; break;
        case '?': path = rec + 2, flags = GIT_UNTRACKED; break;
        case '!': path = rec + 2, flags = GIT_IGNORED; break;
        default: bad = 1; continue;
        }
        if (!path) bad = 1;
        else if (flags) mark_path(&m, path, prefix, flags);
    }
    if (bad) {
        fr_git_marks_free(&m);
        return -1;
    }
    if (m.n > 1) qsort(m.v, (size_t)m.n, sizeof *m.v, by_name);
    int k = 0;
    for (int i = 0; i < m.n; i++) {
        if (k && !strcmp(m.v[k - 1].name, m.v[i].name)) {
            m.v[k - 1].flags |= m.v[i].flags;
            free(m.v[i].name);
        } else m.v[k++] = m.v[i];
    }
    m.n = k;
    fr_git_marks_free(marks);
    *marks = m;
    return 0;
}

int fr_git_flags(const fr_git_marks *marks, const char *name)
{
    int all = marks->n && !*marks->v[0].name ? marks->v[0].flags : 0;
    int lo = 0, hi = marks->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(marks->v[mid].name, name);
        if (!c) return all | marks->v[mid].flags;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return all;
}

void fr_git_marks_free(fr_git_marks *marks)
{
    for (int i = 0; i < marks->n; i++) free(marks->v[i].name);
    free(marks->v);
    *marks = (fr_git_marks){0};
}

char *fr_git_top(const char *dir)
{
    char *d = fr_strdup(dir);
    size_t n = strlen(d);
    while (n > 1 && d[n - 1] == '/') d[--n] = 0;
    for (;;) {
        int root = !strcmp(d, "/");
        char *dot = fr_format("%s%s.git", d, root ? "" : "/");
        struct stat st;
        int found = !lstat(dot, &st);
        free(dot);
        if (found) return d;
        char *slash = strrchr(d, '/');
        if (root || !slash) break;
        if (slash == d) d[1] = 0;
        else *slash = 0;
    }
    free(d);
    return NULL;
}

int fr_git_available(void)
{
    static int known = -1;
    if (known >= 0) return known;
    known = 0;
    const char *path = getenv("PATH");
    char *copy = fr_strdup(path ? path : "/usr/bin:/bin"), *save = NULL;
    for (char *d = strtok_r(copy, ":", &save); d && !known; d = strtok_r(NULL, ":", &save)) {
        char *git = fr_format("%s/git", *d ? d : ".");
        known = !access(git, X_OK);
        free(git);
    }
    free(copy);
    return known;
}

/* the environment without git's own variables (a GIT_DIR from the shell would point git elsewhere), and with
 * optional locks off (git(1), GIT_OPTIONAL_LOCKS) and no prompting */
static char **child_env(void)
{
    int n = 0;
    while (environ && environ[n]) n++;
    char **env = fr_realloc(NULL, sizeof *env * (size_t)(n + 3)), **o = env;
    for (int i = 0; i < n; i++)
        if (strncmp(environ[i], "GIT_", 4)) *o++ = environ[i];
    *o++ = "GIT_OPTIONAL_LOCKS=0";
    *o++ = "GIT_TERMINAL_PROMPT=0";
    *o = NULL;
    return env;
}

int fr_git_status(const char *dir, int timeout_ms, fr_git_marks *marks)
{
    if (!fr_git_available()) return -ENOENT;
    char *top = fr_git_top(dir);
    if (!top) return -ENOENT;
    const char *prefix = dir + strlen(top);
    while (*prefix == '/') prefix++;
    char *pre = fr_strdup(prefix);
    size_t pl = strlen(pre);
    while (pl && pre[pl - 1] == '/') pre[--pl] = 0;
    free(top);

    int fds[2];
    if (pipe2(fds, O_CLOEXEC)) return free(pre), -errno;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    /* core.fsmonitor names a program a repository's config may set: never run one from a folder being browsed */
    char *argv[] = {"git", "--no-optional-locks", "-c", "core.fsmonitor=false", "-C", (char *)dir, "status",
                    "--porcelain=v2", "-z", "--ignored=matching", "--ignore-submodules=all", "--", ".", NULL};
    char **env = child_env();
    pid_t pid;
    int rc = posix_spawnp(&pid, "git", &fa, NULL, argv, env);
    posix_spawn_file_actions_destroy(&fa);
    free(env);
    close(fds[1]);
    if (rc) {
        close(fds[0]);
        free(pre);
        return rc == ENOENT ? -ENOENT : -EIO;
    }
    struct {
        char *v;
        int n, cap;
    } buf = {0};
    double deadline = fr_now() + timeout_ms / 1000.0;
    int timed_out = 0;
    for (;;) {
        int left = (int)((deadline - fr_now()) * 1000);
        if (left <= 0) {
            timed_out = 1;
            break;
        }
        struct pollfd p = {fds[0], POLLIN, 0};
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            timed_out = r == 0;
            break;
        }
        FR_GROW(buf, buf.n + 65536);
        ssize_t got = read(fds[0], buf.v + buf.n, 65536);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        buf.n += (int)got;
        if (buf.n > 64 << 20) {
            timed_out = 1;
            break;
        }
    }
    close(fds[0]);
    if (timed_out) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (timed_out) rc = -ETIMEDOUT;
    else if (!WIFEXITED(status) || WEXITSTATUS(status)) rc = -EIO;
    else rc = fr_git_parse(buf.v ? buf.v : "", (size_t)buf.n, pre, marks) ? -EIO : 0;
    free(buf.v);
    free(pre);
    return rc;
}

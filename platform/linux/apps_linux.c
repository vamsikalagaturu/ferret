/* Open with and Terminal here on the desktop: applications from desktop entries in $XDG_DATA_HOME and
 * $XDG_DATA_DIRS (Desktop Entry Specification 1.5), defaults from mimeapps.list in the order of the MIME
 * Applications Associations specification 1.0.1, types and their parents from shared-mime-info. Apps start from
 * their Exec line with no shell between. */
#define _GNU_SOURCE
#include "core/xdg.h"
#include "platform.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "re");
    if (!f) return NULL;
    struct {
        char *v;
        int n, cap;
    } b = {0};
    char chunk[8192];
    size_t n;
    FR_GROW(b, 1);
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0 && b.n < (4 << 20)) {
        FR_GROW(b, b.n + (int)n + 1);
        memcpy(b.v + b.n, chunk, n);
        b.n += (int)n;
    }
    b.v[b.n] = 0;
    fclose(f);
    return b.v;
}

/* $<home_var> (else ~/<home_default>), then the ':'-separated $<dirs_var> (else dirs_default) */
static fr_strs base_dirs(const char *home_var, const char *home_default, const char *dirs_var, const char *dirs_default)
{
    fr_strs r = {0};
    const char *h = getenv(home_var), *home = getenv("HOME");
    if (h && *h == '/') FR_PUSH(r, fr_strdup(h));
    else if (home) FR_PUSH(r, fr_format("%s/%s", home, home_default));
    const char *d = getenv(dirs_var);
    if (!d || !*d) d = dirs_default;
    while (*d) {
        const char *end = strchr(d, ':');
        if (!end) end = d + strlen(d);
        if (*d == '/') FR_PUSH(r, fr_strndup(d, (size_t)(end - d)));
        d = *end ? end + 1 : end;
    }
    return r;
}

static fr_strs data_dirs(void) { return base_dirs("XDG_DATA_HOME", ".local/share", "XDG_DATA_DIRS", "/usr/local/share:/usr/share"); }
static fr_strs config_dirs(void) { return base_dirs("XDG_CONFIG_HOME", ".config", "XDG_CONFIG_DIRS", "/etc/xdg"); }

static int in_path(const char *name)
{
    if (strchr(name, '/')) return !access(name, X_OK);
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    while (*path) {
        const char *end = strchr(path, ':');
        if (!end) end = path + strlen(path);
        char *p = fr_format("%.*s/%s", (int)(end - path), path, name);
        int ok = !access(p, X_OK);
        free(p);
        if (ok) return 1;
        path = *end ? end + 1 : end;
    }
    return 0;
}

/* ---- the MIME database ---- */

static char *mime_files(const char *name)
{
    fr_strs dirs = data_dirs();
    char *all = fr_strdup("");
    for (int i = 0; i < dirs.n; i++) {
        char *p = fr_format("%s/mime/%s", dirs.v[i], name), *t = read_text(p);
        if (t) {
            char *joined = fr_format("%s%s\n", all, t);
            free(all);
            all = joined;
        }
        free(t), free(p);
    }
    fr_strs_free(&dirs);
    return all;
}

/* ---- applications ---- */

typedef struct {
    char *id, *path;
    fr_desktop_entry e;
} app;

typedef struct {
    app *v;
    int n, cap;
} apps;

static void apps_free(apps *a)
{
    for (int i = 0; i < a->n; i++) free(a->v[i].id), free(a->v[i].path), fr_desktop_entry_free(&a->v[i].e);
    free(a->v);
    *a = (apps){0};
}

static app *app_find(apps *a, const char *id)
{
    for (int i = 0; i < a->n; i++)
        if (!strcmp(a->v[i].id, id)) return &a->v[i];
    return NULL;
}

/* desktop IDs are the path below applications/ with '/' as '-'; the first directory to have one wins */
static void scan_apps(apps *out, const char *dir, const char *prefix, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char *path = fr_format("%s/%s", dir, de->d_name);
        struct stat st;
        size_t n = strlen(de->d_name);
        int ok = !stat(path, &st);
        if (ok && S_ISDIR(st.st_mode) && depth < 4) {
            char *sub = fr_format("%s%s-", prefix, de->d_name);
            scan_apps(out, path, sub, depth + 1);
            free(sub);
        } else if (ok && S_ISREG(st.st_mode) && n > 8 && !strcmp(de->d_name + n - 8, ".desktop")) {
            char *id = fr_format("%s%s", prefix, de->d_name), *text;
            if (app_find(out, id) || !(text = read_text(path))) free(id);
            else {
                app a = {id, fr_strdup(path), {0}};
                fr_desktop_parse(text, &a.e);
                free(text);
                FR_PUSH(*out, a);
            }
        }
        free(path);
    }
    if (d) closedir(d);
}

static apps all_apps(void)
{
    apps r = {0};
    fr_strs dirs = data_dirs();
    for (int i = 0; i < dirs.n; i++) {
        char *p = fr_format("%s/applications", dirs.v[i]);
        scan_apps(&r, p, "", 0);
        free(p);
    }
    fr_strs_free(&dirs);
    return r;
}

static int usable(const app *a)
{
    return a->e.application && !a->e.hidden && a->e.exec && *a->e.exec && (!a->e.try_exec || in_path(a->e.try_exec));
}

/* every mimeapps.list in the order they count, the user's first */
static fr_strs mimeapps_files(void)
{
    fr_strs r = {0}, desks = {0};
    const char *cur = getenv("XDG_CURRENT_DESKTOP");
    for (const char *d = cur ? cur : ""; *d;) {
        const char *end = strchr(d, ':');
        if (!end) end = d + strlen(d);
        char *name = fr_strndup(d, (size_t)(end - d));
        for (char *c = name; *c; c++)
            if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
        FR_PUSH(desks, name);
        d = *end ? end + 1 : end;
    }
    for (int pass = 0; pass < 2; pass++) {
        fr_strs dirs = pass ? data_dirs() : config_dirs();
        for (int i = 0; i < dirs.n; i++) {
            const char *sub = pass ? "/applications" : "";
            for (int k = 0; k < desks.n; k++) FR_PUSH(r, fr_format("%s%s/%s-mimeapps.list", dirs.v[i], sub, desks.v[k]));
            FR_PUSH(r, fr_format("%s%s/mimeapps.list", dirs.v[i], sub));
        }
        fr_strs_free(&dirs);
    }
    fr_strs_free(&desks);
    return r;
}

static int contains(const fr_strs *s, const char *v)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->v[i], v)) return 1;
    return 0;
}

/* a type an app lists (canonical, or "major/ *") covers one of types */
static int handles(const app *a, const fr_strs *types, const char *aliases)
{
    for (int i = 0; i < a->e.mime_types.n; i++) {
        const char *m = a->e.mime_types.v[i];
        size_t n = strlen(m);
        if (n > 2 && !strcmp(m + n - 2, "/*")) {
            for (int k = 0; k < types->n; k++)
                if (!strncmp(types->v[k], m, n - 1)) return 1;
            continue;
        }
        char *canon = fr_mime_unalias(aliases, m);
        int hit = contains(types, canon);
        free(canon);
        if (hit) return 1;
    }
    return 0;
}

static int by_name(const void *pa, const void *pb)
{
    const fr_app_choice *a = pa, *b = pb;
    if (a->is_default != b->is_default) return b->is_default - a->is_default;
    return strcasecmp(a->name, b->name);
}

int plat_apps_for(const char *mime, fr_app_choices *out)
{
    char *aliases = mime_files("aliases"), *subclasses = mime_files("subclasses"), *canon = fr_mime_unalias(aliases, mime);
    fr_strs types = {0};
    fr_mime_ancestors(subclasses, canon, &types);
    apps all = all_apps();
    fr_strs files = mimeapps_files(), added = {0}, removed = {0};
    char **texts = fr_realloc(NULL, sizeof *texts * (size_t)(files.n ? files.n : 1));
    for (int f = 0; f < files.n; f++) texts[f] = read_text(files.v[f]);
    const char *def = NULL;
    for (int t = 0; t < types.n && !def; t++)
        for (int f = 0; f < files.n && !def; f++) {
            if (!texts[f]) continue;
            fr_strs ids = {0};
            fr_mimeapps_get(texts[f], "Default Applications", types.v[t], &ids);
            for (int k = 0; k < ids.n && !def; k++) {
                app *a = app_find(&all, ids.v[k]);
                if (a && usable(a)) def = a->id;
            }
            fr_strs_free(&ids);
        }
    for (int f = 0; f < files.n; f++)
        for (int t = 0; t < types.n && texts[f]; t++) {
            fr_mimeapps_get(texts[f], "Added Associations", types.v[t], &added);
            fr_mimeapps_get(texts[f], "Removed Associations", types.v[t], &removed);
        }
    for (int i = 0; i < all.n; i++) {
        app *a = &all.v[i];
        int is_def = def && !strcmp(def, a->id);
        if (!usable(a) || (!is_def && contains(&removed, a->id))) continue;
        if (!is_def && !contains(&added, a->id) && !handles(a, &types, aliases)) continue;
        FR_PUSH(*out, ((fr_app_choice){fr_strdup(a->id), fr_strdup(a->e.name ? a->e.name : a->id), is_def}));
    }
    if (out->n > 1) qsort(out->v, (size_t)out->n, sizeof *out->v, by_name);
    for (int f = 0; f < files.n; f++) free(texts[f]);
    free(texts);
    fr_strs_free(&files), fr_strs_free(&added), fr_strs_free(&removed), fr_strs_free(&types);
    apps_free(&all);
    free(aliases), free(subclasses), free(canon);
    return 0;
}

/* ---- starting programs ---- */

/* argv in dir (NULL: ours), double forked so it is not our child; a close-on-exec pipe tells whether exec worked */
static int spawn(const fr_strs *argv, const char *dir)
{
    if (replay_running() || !argv->n) return -1;
    char **args = fr_realloc(NULL, sizeof *args * (size_t)(argv->n + 1));
    for (int i = 0; i < argv->n; i++) args[i] = argv->v[i];
    args[argv->n] = NULL;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC)) return free(args), -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]), close(fds[1]);
        free(args);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        setsid();
        pid_t again = fork();
        if (again != 0) _exit(again < 0);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) dup2(null, 0), dup2(null, 1), dup2(null, 2);
        if (dir && chdir(dir)) {}
        execvp(args[0], args);
        char failed = 1;
        if (write(fds[1], &failed, 1)) {}
        _exit(127);
    }
    close(fds[1]);
    free(args);
    int status = 0;
    waitpid(pid, &status, 0);
    char failed = 0;
    ssize_t n = read(fds[0], &failed, 1);
    close(fds[0]);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && n <= 0 ? 0 : -1;
}

/* How each terminal is told its folder and the command to run. The folder is also the child's working directory,
 * which is all $TERMINAL, xdg-terminal-exec and xterm go by. */
static const struct terminal {
    const char *name, *sub;   /* sub: a subcommand first ("wezterm start") */
    const char *dir, *dir_eq; /* "--flag DIR" or "--flag=DIR" */
    const char *run;          /* before the command; "" for none */
} terminals[] = {
    {"xdg-terminal-exec", NULL, NULL, NULL, ""},
    {"alacritty", NULL, "--working-directory", NULL, "-e"},
    {"kitty", NULL, "--directory", NULL, ""},
    {"foot", NULL, NULL, "--working-directory=", ""},
    {"wezterm", "start", "--cwd", NULL, "--"},
    {"gnome-terminal", NULL, NULL, "--working-directory=", "--"},
    {"konsole", NULL, "--workdir", NULL, "-e"},
    {"xterm", NULL, NULL, NULL, "-e"},
};

/* the terminal's command line, then cmd in it (may be NULL); 0, or -1 when there is no terminal */
static int terminal_argv(const char *dir, const fr_strs *cmd, fr_strs *out)
{
    const char *env = getenv("TERMINAL");
    if (env && *env) {
        fr_strs words = {0};
        if (!fr_exec_split(env, &words) && words.n && in_path(words.v[0])) {
            *out = words;
            if (cmd && cmd->n) FR_PUSH(*out, fr_strdup("-e"));
            for (int i = 0; cmd && i < cmd->n; i++) FR_PUSH(*out, fr_strdup(cmd->v[i]));
            return 0;
        }
        fr_strs_free(&words);
    }
    for (size_t t = 0; t < sizeof terminals / sizeof *terminals; t++) {
        const struct terminal *k = &terminals[t];
        if (!in_path(k->name)) continue;
        FR_PUSH(*out, fr_strdup(k->name));
        if (k->sub) FR_PUSH(*out, fr_strdup(k->sub));
        if (dir && k->dir) {
            FR_PUSH(*out, fr_strdup(k->dir));
            FR_PUSH(*out, fr_strdup(dir));
        }
        if (dir && k->dir_eq) FR_PUSH(*out, fr_format("%s%s", k->dir_eq, dir));
        if (cmd && cmd->n && *k->run) FR_PUSH(*out, fr_strdup(k->run));
        for (int i = 0; cmd && i < cmd->n; i++) FR_PUSH(*out, fr_strdup(cmd->v[i]));
        return 0;
    }
    return -1;
}

int plat_terminal(const char *dir)
{
    fr_strs argv = {0};
    int rc = terminal_argv(dir, NULL, &argv);
    if (!rc && dir) rc = spawn(&argv, dir);
    fr_strs_free(&argv);
    return rc;
}

int plat_open_app(const char *id, const fr_strs *paths)
{
    apps all = all_apps();
    app *a = app_find(&all, id);
    fr_argvs runs = {0};
    int rc = a && usable(a) ? fr_exec_expand(a->e.exec, paths, a->e.name ? a->e.name : id, a->e.icon, a->path, &runs) : -1;
    for (int i = 0; !rc && i < runs.n; i++) {
        if (a->e.terminal) {
            fr_strs wrapped = {0};
            rc = terminal_argv(NULL, &runs.v[i], &wrapped);
            if (!rc) rc = spawn(&wrapped, NULL);
            fr_strs_free(&wrapped);
        } else rc = spawn(&runs.v[i], NULL);
    }
    fr_argvs_free(&runs);
    apps_free(&all);
    return rc;
}

int plat_set_default_app(const char *mime, const char *id)
{
    fr_strs dirs = config_dirs();
    if (!dirs.n) return -ENOENT;
    int rc = fr_mkdirs(dirs.v[0]);
    char *path = fr_format("%s/mimeapps.list", dirs.v[0]);
    fr_strs_free(&dirs);
    char *real = realpath(path, NULL), *target = real ? real : path;
    char *old = read_text(target), *text = fr_mimeapps_set_default(old, mime, id);
    char *tmp = fr_format("%s.ferret-tmp", target);
    FILE *f = rc ? NULL : fopen(tmp, "we");
    if (!rc && !f) rc = -errno;
    if (f) {
        int ok = fputs(text, f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
        if (!ok) rc = -errno;
        if (fclose(f) && !rc) rc = -errno;
        if (!rc && rename(tmp, target)) rc = -errno;
        if (rc) unlink(tmp);
    }
    free(tmp), free(text), free(old), free(real), free(path);
    return rc;
}

int plat_open_chooser(const char *path) { return -1; }

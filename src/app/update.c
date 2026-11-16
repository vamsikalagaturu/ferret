/* Updates from GitHub's latest release of ferret (net/release.h): asked at most once a day on start, and from the
 * menu. A newer version shows a bar; on a phone Update downloads the release's APK and hands it to Android's installer,
 * where the user confirms, and on the desktop it opens the release's page. */
#include "app.h"
#include "jobs/async.h"
#include "net/release.h"
#include "vfs/vfs_curl.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define CHECK_EVERY_S (24 * 3600)

static struct {
    int checking, downloading, dismissed;
    int installed; /* the Linux program was replaced; a restart runs the new one */
    fr_release rel; /* a newer release than this build, tag NULL when none */
    volatile float progress; /* of the download, 0..1, written by its worker thread */
} U;

typedef struct {
    int manual, rc;
    fr_release rel;
    char *path; /* a download's file */
} task;

/* ~/.cache/ferret/update (the app's cache folder on a phone); its mtime is when the last check was */
static char *update_dir(void)
{
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && xdg[0] == '/') return fr_format("%s/ferret/update", xdg);
    return fr_format("%s/.cache/ferret/update", home && *home ? home : "/tmp");
}

static int checked_lately(void)
{
    char *dir = update_dir();
    struct stat st;
    int recent = !stat(dir, &st) && time(NULL) - st.st_mtime < CHECK_EVERY_S;
    free(dir);
    return recent;
}

static void mark_checked(void)
{
    char *dir = update_dir();
    fr_mkdirs(dir);
    utime(dir, NULL);
    free(dir);
}

static void check_work(void *arg)
{
    task *t = arg;
    t->rc = fr_release_latest(&t->rel);
}

static void check_done(void *arg)
{
    task *t = arg;
    U.checking = 0;
    if (!t->rc) mark_checked();
    if (!t->rc && fr_version_cmp(t->rel.tag, FR_VERSION) > 0) {
        fr_release_free(&U.rel);
        U.rel = t->rel;
        U.dismissed = 0;
    } else {
        if (t->manual) ui_toast(t->rc ? "Could not check for updates" : "ferret %s is the latest", FR_VERSION);
        fr_release_free(&t->rel);
    }
    free(t);
    ui.dirty = 1;
}

void update_check(int manual)
{
    if (U.checking || replay_running() || (!manual && checked_lately())) return;
    U.checking = 1;
    task *t = fr_realloc(NULL, sizeof *t);
    *t = (task){.manual = manual};
    fr_async_run(check_work, check_done, t);
}

/* The installed Linux program (~/.local/bin/ferret, platform/linux/install.sh), which an update replaces; NULL
 * for a build run from elsewhere, such as the build folder, which only gets the release page. */
static char *installed_program(void)
{
#ifdef __ANDROID__
    return NULL;
#else
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    const char *home = getenv("HOME");
    if (n <= 0 || !home) return NULL;
    self[n] = 0;
    char *want = fr_format("%s/.local/bin/ferret", home);
    if (strcmp(self, want)) free(want), want = NULL;
    return want;
#endif
}

static void download_work(void *arg)
{
    task *t = arg;
    const char *url = t->rel.apk ? t->rel.apk : t->rel.linux_bin;
    t->rc = fr_curl_fetch(url, NULL, t->path, &U.progress);
#ifndef __ANDROID__
    /* next to the program, then renamed over it: the running one keeps its inode until it exits */
    if (!t->rc && t->rel.linux_bin) {
        char *prog = installed_program();
        if (!prog) t->rc = -ENOENT;
        else if (chmod(t->path, 0755) || rename(t->path, prog)) t->rc = -errno;
        free(prog);
    }
#endif
}

static void download_done(void *arg)
{
    task *t = arg;
    U.downloading = 0;
    if (t->rc) ui_toast("Could not install the update");
    else if (t->rel.apk && plat_open(t->path)) ui_toast("Android did not open the installer");
    else if (t->rel.linux_bin) U.installed = 1;
    free(t->path), fr_release_free(&t->rel), free(t);
    ui.dirty = 1;
}

static void restart(void)
{
#ifndef __ANDROID__
    char *prog = installed_program();
    if (!prog) return;
    settings_flush();
    execv(prog, (char *[]){prog, NULL});
    ui_toast("Could not restart: %s", strerror(errno));
    free(prog);
#endif
}

static void update_now(void)
{
    char *prog = installed_program();
    int android = ui.touch && U.rel.apk, desktop = prog && U.rel.linux_bin;
    if (!android && !desktop) {
        free(prog);
        if (U.rel.page && plat_open(U.rel.page)) ui_toast("Could not open %s", U.rel.page);
        return;
    }
    char *dir = android ? update_dir() : fr_strdup(prog);
    if (!android) *strrchr(dir, '/') = 0;
    fr_mkdirs(dir);
    task *t = fr_realloc(NULL, sizeof *t);
    *t = (task){.path = android ? fr_format("%s/ferret-%s.apk", dir, U.rel.tag) : fr_format("%s/.ferret-%s.new", dir, U.rel.tag)};
    if (android) t->rel.apk = fr_strdup(U.rel.apk);
    else t->rel.linux_bin = fr_strdup(U.rel.linux_bin);
    free(dir), free(prog);
    U.downloading = 1;
    U.progress = 0;
    fr_async_run(download_work, download_done, t);
}

static int about_open;

void update_about_open(void) { about_open = 1; }

/* ferret's version, and the way to the updates: the check (whose outcome is the bar or a toast) and the releases */
void update_about_draw(void)
{
    if (!about_open) return;
    char message[160];
    snprintf(message, sizeof message, "Version %s\nA file explorer for Linux and Android.", FR_VERSION);
    fr_dialog d = {.title = "About ferret", .message = message,
                   .buttons = {"Releases", "Close", U.checking ? "Checking..." : "Check for updates"}, .nbuttons = 3,
                   .cancel = 1};
    int b = ui_dialog(&d);
    if (b == 0 && plat_open("https://github.com/" FR_RELEASE_REPO "/releases"))
        ui_toast("Could not open the releases page");
    if (b == 2 && !U.checking) update_check(1);
    if (b >= 0) about_open = 0;
}

float update_bar_draw(Rectangle area)
{
    if (!U.rel.tag || U.dismissed) return 0;
    float h = DP(ui.touch ? 64 : 52), pad = DP(12), bh = DP(40);
    Rectangle r = {area.x, area.y + area.height - h, area.width, h};
    ui_rect(r, theme.surface);
    ui_hline(r.x, r.y, r.width, theme.border);
    ui_icon("download", (Rectangle){r.x + pad, r.y, DP(32), h}, 20, theme.accent);
    char label[96];
    const char *v = U.rel.tag[0] == 'v' ? U.rel.tag + 1 : U.rel.tag;
    if (U.installed) snprintf(label, sizeof label, "ferret %s is installed", v);
    else if (U.downloading) snprintf(label, sizeof label, "Downloading ferret %s... %d%%", v, (int)(U.progress * 100));
    else snprintf(label, sizeof label, "ferret %s is available", v);
    if (U.downloading) {
        /* a thin track along the bar's foot, filled as the worker reports; the frame loop idles, so ask again */
        ui_progress((Rectangle){r.x + pad + DP(40), r.y + h - DP(10), r.width - 2 * pad - DP(40), DP(4)}, U.progress,
                    theme.accent);
        ui_redraw_in(0.1);
    }
    char *prog = installed_program();
    const char *go = U.installed ? "Restart" : (ui.touch && U.rel.apk) || (prog && U.rel.linux_bin) ? "Update" : "Open";
    free(prog);
    float uw = ui_button_width(go), lw = ui_button_width("Later");
    ui_text_box(label, (Rectangle){r.x + pad + DP(40), r.y, r.width - 2 * pad - DP(48) - uw - lw - DP(8), h}, 15, FONT_REGULAR, theme.text,
                ALIGN_LEFT, 0);
    if (!U.downloading) {
        float bx = r.x + r.width - pad - uw;
        if (ui_button(ui_id("update", 0), (Rectangle){bx, r.y + (h - bh) / 2, uw, bh}, go, 1)) U.installed ? restart() : update_now();
        if (ui_button(ui_id("update-later", 0), (Rectangle){bx - DP(8) - lw, r.y + (h - bh) / 2, lw, bh}, "Later", 0)) U.dismissed = 1;
    }
    return h;
}

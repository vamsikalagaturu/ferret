/* The desktop side of platform.h: GLFW through raylib (Wayland or X11), xdg-open, XDG folders; disks are in disks_linux.c. */
#define _GNU_SOURCE
#include "platform.h"
#include "vfs/vfs.h"
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

void *glfwGetCurrentContext(void);
int glfwGetPlatform(void);
void glfwInitHint(int hint, int value);
void glfwGetWindowContentScale(void *window, float *x, float *y);
void glfwGetFramebufferSize(void *window, int *width, int *height);
void glfwWaitEventsTimeout(double timeout);
void glfwPostEmptyEvent(void);
typedef void (*glfw_size_fn)(void *window, int width, int height);
glfw_size_fn glfwSetFramebufferSizeCallback(void *window, glfw_size_fn fn);
#define GLFW_PLATFORM 0x00050003
#define GLFW_PLATFORM_WAYLAND 0x00060003
#define GLFW_PLATFORM_X11 0x00060004

static int wayland(void) { return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND; }

static float content_scale(void)
{
    float x = 1, y = 1;
    glfwGetWindowContentScale(glfwGetCurrentContext(), &x, &y);
    return x > 0 ? x : 1;
}

/* raylib takes the size it asked for as the framebuffer's and then hears only of changes, so a Wayland scale that
 * arrives while the window is made would leave the UI filling part of it. */
static void adopt_framebuffer_size(void)
{
    void *w = glfwGetCurrentContext();
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(w, &fw, &fh);
    if (fw == GetRenderWidth() && fh == GetRenderHeight()) return;
    glfw_size_fn raylib_size = glfwSetFramebufferSizeCallback(w, NULL);
    glfwSetFramebufferSizeCallback(w, raylib_size);
    if (raylib_size) raylib_size(w, fw, fh);
}

/* worker threads (jobs, properties, disks) wake the window; none may reach GLFW once it is gone */
static pthread_mutex_t wake_lock = PTHREAD_MUTEX_INITIALIZER;
static int window_alive;

void plat_init_window(void)
{
    SetTraceLogLevel(LOG_ERROR);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    /* GLFW alone goes by XDG_SESSION_TYPE and does not fall back to X11 when Wayland is unreachable */
    const char *wl = getenv("WAYLAND_DISPLAY");
    glfwInitHint(GLFW_PLATFORM, wl && *wl ? GLFW_PLATFORM_WAYLAND : GLFW_PLATFORM_X11);
    InitWindow(1100, 760, "ferret");
    if (!IsWindowReady()) {
        fprintf(stderr, "ferret: cannot open a window (no Wayland or X11 display?)\n");
        exit(1);
    }
    SetExitKey(KEY_NULL);
    pthread_mutex_lock(&wake_lock);
    window_alive = 1;
    pthread_mutex_unlock(&wake_lock);
    int hz = GetMonitorRefreshRate(GetCurrentMonitor());
    SetTargetFPS(hz >= 30 ? hz : 60);
    if (wayland()) adopt_framebuffer_size();
    else {
        float s = plat_scale();
        int mon = GetCurrentMonitor();
        if (s > 1.01f)
            SetWindowSize((int)fminf(1100 * s, GetMonitorWidth(mon) * 0.9f), (int)fminf(760 * s, GetMonitorHeight(mon) * 0.9f));
    }
}

void plat_frame(void)
{
    if (wayland()) adopt_framebuffer_size();
}

void plat_wait(double seconds) { glfwWaitEventsTimeout(seconds); }
void plat_wake(void)
{
    pthread_mutex_lock(&wake_lock);
    if (window_alive) glfwPostEmptyEvent();
    pthread_mutex_unlock(&wake_lock);
}

void plat_close_window(void)
{
    pthread_mutex_lock(&wake_lock);
    window_alive = 0;
    pthread_mutex_unlock(&wake_lock);
    CloseWindow();
}

/* Wayland's content scale; on X11 the content scale when set (Xft.dpi), else the monitor's physical DPI over 96
 * in quarter steps */
float plat_scale(void)
{
    if (wayland()) return content_scale() > 1 ? content_scale() : 1;
    Vector2 c = GetWindowScaleDPI();
    if (c.x > 1.01f) return c.x;
    int mon = GetCurrentMonitor();
    float wpx = (float)GetMonitorWidth(mon), hpx = (float)GetMonitorHeight(mon);
    float wmm = (float)GetMonitorPhysicalWidth(mon), hmm = (float)GetMonitorPhysicalHeight(mon);
    if (wmm < 50 || hmm < 50) return 1;
    float dpi = sqrtf(wpx * wpx + hpx * hpx) / (sqrtf(wmm * wmm + hmm * hmm) / 25.4f);
    float s = roundf(dpi / 96 * 4) / 4;
    return s < 1 ? 1 : s > 3 ? 3 : s;
}

/* Wayland reports the pointer in logical units while the framebuffer is in pixels */
Vector2 plat_mouse(void)
{
    if (replay_running()) return replay_pointer();
    Vector2 m = GetMousePosition();
    if (wayland()) {
        float s = content_scale();
        m.x *= s, m.y *= s;
    }
    return m;
}

int plat_touch(void) { return 0; }
void plat_insets(float *l, float *t, float *r, float *b) { *l = *t = *r = *b = 0; }
void plat_keyboard(int show) { (void)show; }
void plat_haptic(void) {}
int plat_storage_granted(void) { return 1; }
void plat_request_storage(void) {}

static char *replay_clipboard;

void plat_set_clipboard(const char *text)
{
    if (!replay_running()) {
        SetClipboardText(text);
        return;
    }
    free(replay_clipboard);
    replay_clipboard = fr_strdup(text);
}

char *plat_clipboard(void)
{
    const char *s = replay_running() ? replay_clipboard : GetClipboardText();
    return s && *s ? fr_strdup(s) : NULL;
}

/* Double fork so the opened app is not our child; a close-on-exec pipe tells whether exec worked. */
int plat_share(const char *paths)
{
    (void)paths;
    return -1;
}

int plat_open(const char *path)
{
    if (replay_running()) return -1;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]), close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        setsid();
        pid_t again = fork();
        if (again != 0) _exit(again < 0);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) dup2(null, 0), dup2(null, 1), dup2(null, 2);
        execlp("xdg-open", "xdg-open", path, (char *)NULL);
        char failed = 1;
        if (write(fds[1], &failed, 1)) {}
        _exit(127);
    }
    close(fds[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    char failed = 0;
    ssize_t n = read(fds[0], &failed, 1);
    close(fds[0]);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && n <= 0 ? 0 : -1;
}

static void add(fr_places *p, const char *label, const char *path, const char *icon)
{
    struct stat st;
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) return;
    for (int i = 0; i < p->n; i++)
        if (fr_uri_local(p->v[i].uri) && !strcmp(fr_uri_local(p->v[i].uri), path)) return;
    FR_PUSH(*p, ((fr_place){fr_strdup(label), fr_uri_from_path(path), fr_strdup(icon), 0}));
}

/* $XDG_<NAME>_DIR from ~/.config/user-dirs.dirs ("$HOME/Downloads"), else ~/<fallback> */
static char *user_dir(const char *home, const char *key, const char *fallback)
{
    const char *cfg = getenv("XDG_CONFIG_HOME");
    char *path = cfg && *cfg == '/' ? fr_format("%s/user-dirs.dirs", cfg) : fr_format("%s/.config/user-dirs.dirs", home);
    FILE *f = fopen(path, "r");
    free(path);
    char *r = NULL, line[1024];
    size_t kl = strlen(key);
    while (f && !r && fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) || line[kl] != '=') continue;
        char *v = line + kl + 1, *end;
        if (*v == '"') v++;
        if ((end = strchr(v, '"'))) *end = 0;
        else v[strcspn(v, "\n")] = 0;
        if (!strncmp(v, "$HOME", 5)) r = fr_format("%s%s", home, v + 5);
        else if (*v == '/') r = fr_strdup(v);
    }
    if (f) fclose(f);
    return r ? r : fr_format("%s/%s", home, fallback);
}

void plat_places(fr_places *out)
{
    const char *home = getenv("HOME");
    if (home) {
        add(out, "~", home, "house");
        static const struct {
            const char *key, *fallback, *icon;
        } dirs[] = {{"XDG_DESKTOP_DIR", "Desktop", "monitor"},  {"XDG_DOCUMENTS_DIR", "Documents", "file-text"},
                    {"XDG_DOWNLOAD_DIR", "Downloads", "download"}, {"XDG_PICTURES_DIR", "Pictures", "image"},
                    {"XDG_MUSIC_DIR", "Music", "music"},        {"XDG_VIDEOS_DIR", "Videos", "video"}};
        for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
            char *p = user_dir(home, dirs[i].key, dirs[i].fallback);
            if (strcmp(p, home)) add(out, strrchr(p, '/') + 1, p, dirs[i].icon);
            free(p);
        }
        FR_PUSH(*out, ((fr_place){fr_strdup("Trash"), fr_strdup("trash:///"), fr_strdup("trash-2"), 0}));
    }
    add(out, "/", "/", "hard-drive");
}

char *plat_home_uri(void)
{
    const char *home = getenv("HOME");
    return fr_uri_from_path(home ? home : "/");
}

int main(int argc, char **argv) { return fr_app_main(argc, argv); }

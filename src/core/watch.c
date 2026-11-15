#define _GNU_SOURCE
#include "watch.h"
#include "util.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/vfs.h>
#include <unistd.h>

#define MASK (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR)

typedef struct {
    int wd, refs, pending, gone;
    unsigned serial;
} watch;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    watch *v;
    int n, cap;
} watches;
static int ifd = -1, stop_pipe[2] = {-1, -1}, started;
static pthread_t thread;
static void (*wake_fn)(void);

static watch *find(int wd)
{
    for (int i = 0; i < watches.n; i++)
        if (watches.v[i].wd == wd) return &watches.v[i];
    return NULL;
}

/* statfs(2) f_type values (linux/magic.h, and CIFS's and SMB2's from the kernel's fs/smb) */
static int remote_fs(const char *path)
{
    struct statfs s;
    if (statfs(path, &s)) return 1;
    static const unsigned long remote[] = {0x6969 /* NFS */,          0x517B /* SMB */,  0xFF534D42 /* CIFS */,
                                           0xFE534D42 /* SMB2 */,     0x65735546 /* FUSE */, 0x01021997 /* 9P */,
                                           0x00C36400 /* Ceph */,     0x5346414F /* AFS */, 0x6B414653 /* kAFS */};
    for (size_t i = 0; i < sizeof remote / sizeof *remote; i++)
        if ((unsigned long)(unsigned)s.f_type == remote[i]) return 1;
    return 0;
}

static void mark(int wd, uint32_t mask)
{
    if (mask & IN_Q_OVERFLOW) {
        for (int i = 0; i < watches.n; i++) watches.v[i].pending = 1;
        return;
    }
    watch *w = find(wd);
    if (!w) return;
    w->pending = 1;
    if (mask & IN_IGNORED) w->gone = 1;
}

static void *run(void *arg)
{
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
    double deadline = 0;
    for (;;) {
        int timeout = -1;
        if (deadline) {
            double left = deadline - fr_now();
            timeout = left <= 0 ? 0 : (int)(left * 1000) + 1;
        }
        struct pollfd p[2] = {{ifd, POLLIN, 0}, {stop_pipe[0], POLLIN, 0}};
        int r = poll(p, 2, timeout);
        if (r < 0 && errno != EINTR) break;
        if (r > 0 && p[1].revents) break;
        if (r > 0 && p[0].revents & POLLIN) {
            ssize_t n;
            int any = 0;
            while ((n = read(ifd, buf, sizeof buf)) > 0) {
                pthread_mutex_lock(&lock);
                for (char *q = buf; q < buf + n;) {
                    const struct inotify_event *ev = (const struct inotify_event *)q;
                    mark(ev->wd, ev->mask);
                    q += sizeof *ev + ev->len;
                }
                pthread_mutex_unlock(&lock);
                any = 1;
            }
            if (any && !deadline) deadline = fr_now() + FR_WATCH_COALESCE_MS / 1000.0;
        }
        if (deadline && fr_now() >= deadline) {
            deadline = 0;
            int told = 0;
            pthread_mutex_lock(&lock);
            for (int i = 0; i < watches.n; i++)
                if (watches.v[i].pending) watches.v[i].pending = 0, watches.v[i].serial++, told = 1;
            pthread_mutex_unlock(&lock);
            if (told && wake_fn) wake_fn();
        }
    }
    return NULL;
}

int fr_watch_start(void (*wake)(void))
{
    if (started) return 0;
    ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) return -1;
    if (pipe2(stop_pipe, O_CLOEXEC)) {
        close(ifd);
        ifd = -1;
        return -1;
    }
    wake_fn = wake;
    if (pthread_create(&thread, NULL, run, NULL)) {
        close(ifd), close(stop_pipe[0]), close(stop_pipe[1]);
        ifd = stop_pipe[0] = stop_pipe[1] = -1;
        return -1;
    }
    started = 1;
    return 0;
}

void fr_watch_stop(void)
{
    if (!started) return;
    char c = 0;
    while (write(stop_pipe[1], &c, 1) < 0 && errno == EINTR) {
    }
    pthread_join(thread, NULL);
    close(ifd), close(stop_pipe[0]), close(stop_pipe[1]);
    ifd = stop_pipe[0] = stop_pipe[1] = -1;
    pthread_mutex_lock(&lock);
    free(watches.v);
    watches.v = NULL;
    watches.n = watches.cap = 0;
    pthread_mutex_unlock(&lock);
    wake_fn = NULL;
    started = 0;
}

int fr_watch_add(const char *path)
{
    if (!started || !path || remote_fs(path)) return -1;
    pthread_mutex_lock(&lock);
    int wd = inotify_add_watch(ifd, path, MASK);
    if (wd >= 0) {
        watch *w = find(wd);
        if (w && w->gone) *w = (watch){.wd = wd, .refs = w->refs + 1, .serial = w->serial};
        else if (w) w->refs++;
        else FR_PUSH(watches, ((watch){.wd = wd, .refs = 1}));
    }
    pthread_mutex_unlock(&lock);
    return wd >= 0 ? wd : -1;
}

void fr_watch_remove(int handle)
{
    if (handle < 0) return;
    pthread_mutex_lock(&lock);
    watch *w = started ? find(handle) : NULL;
    if (w && --w->refs <= 0) {
        if (!w->gone) inotify_rm_watch(ifd, w->wd);
        *w = watches.v[--watches.n];
    }
    pthread_mutex_unlock(&lock);
}

unsigned fr_watch_serial(int handle)
{
    pthread_mutex_lock(&lock);
    watch *w = handle >= 0 ? find(handle) : NULL;
    unsigned s = w ? w->serial : 0;
    pthread_mutex_unlock(&lock);
    return s;
}

int fr_watch_gone(int handle)
{
    pthread_mutex_lock(&lock);
    watch *w = handle >= 0 ? find(handle) : NULL;
    int gone = !w || w->gone;
    pthread_mutex_unlock(&lock);
    return gone;
}

#else
int fr_watch_start(void (*wake)(void)) { return -1; }
void fr_watch_stop(void) {}
int fr_watch_add(const char *path) { return -1; }
void fr_watch_remove(int handle) {}
unsigned fr_watch_serial(int handle) { return 0; }
int fr_watch_gone(int handle) { return 1; }
#endif

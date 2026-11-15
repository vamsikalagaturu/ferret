/* src/core/watch.c: a burst of changes in a watched folder is told once, a shared watch outlives one removal, and a
 * folder that goes is told too */
#define _GNU_SOURCE
#include "check.h"
#include "core/util.h"
#include "core/watch.h"
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int wakes;

static void wake(void) { __atomic_add_fetch(&wakes, 1, __ATOMIC_SEQ_CST); }

static int woken(void) { return __atomic_load_n(&wakes, __ATOMIC_SEQ_CST); }

static void sleep_ms(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

/* until the serial moves past was, at most a second */
static unsigned wait_serial(int h, unsigned was)
{
    for (int i = 0; i < 100 && fr_watch_serial(h) == was; i++) sleep_ms(10);
    return fr_watch_serial(h);
}

static void touch(const char *dir, const char *name)
{
    char *p = fr_format("%s/%s", dir, name);
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    free(p);
}

int main(void)
{
#if defined(__linux__) && !defined(__ANDROID__)
    char tmpl[] = "/tmp/ferret-watch-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    CHECK(fr_watch_add(tmpl) == -1); /* not started */
    CHECK(fr_watch_start(wake) == 0);
    int h = fr_watch_add(tmpl);
    CHECK(h >= 0);
    unsigned s0 = fr_watch_serial(h);

    /* a burst: many files at once, one serial step and one wake */
    for (int i = 0; i < 200; i++) {
        char name[32];
        snprintf(name, sizeof name, "f%d", i);
        touch(tmpl, name);
    }
    unsigned s1 = wait_serial(h, s0);
    CHECK(s1 == s0 + 1);
    sleep_ms(3 * FR_WATCH_COALESCE_MS);
    CHECK(fr_watch_serial(h) == s1);
    CHECK(woken() == 1);

    /* nothing changes: nothing told */
    sleep_ms(2 * FR_WATCH_COALESCE_MS);
    CHECK(fr_watch_serial(h) == s1 && woken() == 1);

    /* two panes on one folder share the watch; one leaving keeps it for the other */
    int h2 = fr_watch_add(tmpl);
    CHECK(h2 == h);
    fr_watch_remove(h2);
    touch(tmpl, "later");
    unsigned s2 = wait_serial(h, s1);
    CHECK(s2 == s1 + 1);
    CHECK(!fr_watch_gone(h));

    /* the folder goes */
    for (int i = 0; i < 200; i++) {
        char *p = fr_format("%s/f%d", tmpl, i);
        unlink(p);
        free(p);
    }
    char *later = fr_format("%s/later", tmpl);
    unlink(later);
    free(later);
    sleep_ms(3 * FR_WATCH_COALESCE_MS);
    unsigned s3 = fr_watch_serial(h);
    CHECK(rmdir(tmpl) == 0);
    unsigned s4 = wait_serial(h, s3);
    CHECK(s4 > s3);
    for (int i = 0; i < 100 && !fr_watch_gone(h); i++) sleep_ms(10);
    CHECK(fr_watch_gone(h));
    fr_watch_remove(h);
    fr_watch_stop();
    int after = woken();
    sleep_ms(2 * FR_WATCH_COALESCE_MS);
    CHECK(woken() == after);
#endif
    return CHECK_DONE();
}

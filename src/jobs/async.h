#ifndef FR_ASYNC_H
#define FR_ASYNC_H
/* Work that must not hold up the window, such as listing a folder on a server: work(arg) runs on a thread of its
 * own, then done(arg) runs on the window's thread in fr_async_poll(), which owns arg from then on. Each task has
 * its own thread, so a server that does not answer holds up only what asked it. */
#include "vfs/vfs.h"

/* wake: called from a task's thread when it finished (the window sleeps between events); NULL in tests */
void fr_async_start(void (*wake)(void));
void fr_async_run(void (*work)(void *arg), void (*done)(void *arg), void *arg);
/* window thread: runs the done of every task that finished, in the order they finished; how many ran */
int fr_async_poll(void);
/* tests: waits until every task's work has finished */
void fr_async_wait(void);

/* A folder listed that way: done gets the URI, the entries (it may take them, leaving *entries empty) and 0 or
 * -errno. */
typedef void (*fr_async_list_done)(const char *uri, fr_entries *entries, int rc, void *ctx);
void fr_async_list(const char *uri, fr_async_list_done done, void *ctx);
#endif

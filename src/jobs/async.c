#include "async.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct {
    void (*work)(void *), (*done)(void *);
    void *arg;
} task;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle = PTHREAD_COND_INITIALIZER;
static struct {
    task *v;
    int n, cap;
} finished;
static int running;
static void (*wake_fn)(void);

void fr_async_start(void (*wake)(void))
{
    pthread_mutex_lock(&lock);
    wake_fn = wake;
    pthread_mutex_unlock(&lock);
}

static void *run(void *p)
{
    task t = *(task *)p;
    free(p);
    t.work(t.arg);
    pthread_mutex_lock(&lock);
    FR_PUSH(finished, t);
    running--;
    pthread_cond_broadcast(&idle);
    void (*wake)(void) = wake_fn;
    pthread_mutex_unlock(&lock);
    if (wake) wake();
    return NULL;
}

void fr_async_run(void (*work)(void *arg), void (*done)(void *arg), void *arg)
{
    task *t = fr_realloc(NULL, sizeof *t);
    *t = (task){work, done, arg};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    pthread_mutex_lock(&lock);
    running++;
    pthread_mutex_unlock(&lock);
    if (pthread_create(&th, &attr, run, t)) {
        /* no thread to be had: the work runs here, late rather than never */
        free(t);
        work(arg);
        pthread_mutex_lock(&lock);
        FR_PUSH(finished, ((task){work, done, arg}));
        running--;
        pthread_cond_broadcast(&idle);
        pthread_mutex_unlock(&lock);
    }
    pthread_attr_destroy(&attr);
}

int fr_async_poll(void)
{
    int ran = 0;
    for (;;) {
        pthread_mutex_lock(&lock);
        if (!finished.n) {
            pthread_mutex_unlock(&lock);
            return ran;
        }
        task t = finished.v[0];
        for (int i = 1; i < finished.n; i++) finished.v[i - 1] = finished.v[i];
        finished.n--;
        pthread_mutex_unlock(&lock);
        t.done(t.arg);
        ran++;
    }
}

void fr_async_wait(void)
{
    pthread_mutex_lock(&lock);
    while (running) pthread_cond_wait(&idle, &lock);
    pthread_mutex_unlock(&lock);
}

typedef struct {
    char *uri;
    fr_entries es;
    int rc;
    fr_async_list_done done;
    void *ctx;
} listing;

static void list_work(void *p)
{
    listing *l = p;
    l->rc = fr_vfs_list(l->uri, &l->es);
}

static void list_done(void *p)
{
    listing *l = p;
    l->done(l->uri, &l->es, l->rc, l->ctx);
    fr_entries_free(&l->es);
    free(l->uri);
    free(l);
}

void fr_async_list(const char *uri, fr_async_list_done done, void *ctx)
{
    listing *l = fr_realloc(NULL, sizeof *l);
    *l = (listing){.uri = fr_strdup(uri), .done = done, .ctx = ctx};
    fr_async_run(list_work, list_done, l);
}

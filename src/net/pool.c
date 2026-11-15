#include "net.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

struct fr_conn {
    char *key;
    const fr_conn_ops *ops;
    pthread_mutex_t lock; /* held while a thread uses the session */
    void *session;
    unsigned generation; /* grows with each session, so streams know theirs went */
    int refs;            /* under pool_lock */
};

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    fr_conn **v;
    int n, cap;
} pool;

/* a server closing the socket under a write must fail that write, not end the process */
static void ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }

fr_conn *fr_conn_get(const char *key, const fr_net_uri *u, const fr_conn_ops *ops, int *fresh, int *err)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, ignore_sigpipe);
    if (fresh) *fresh = 0;
    pthread_mutex_lock(&pool_lock);
    fr_conn *c = NULL;
    for (int i = 0; i < pool.n && !c; i++)
        if (!strcmp(pool.v[i]->key, key)) c = pool.v[i];
    if (!c) {
        c = fr_realloc(NULL, sizeof *c);
        *c = (fr_conn){.key = fr_strdup(key), .ops = ops};
        pthread_mutex_init(&c->lock, NULL);
        FR_PUSH(pool, c);
    }
    c->refs++;
    pthread_mutex_unlock(&pool_lock);
    pthread_mutex_lock(&c->lock);
    if (!c->session) {
        int e = -EIO;
        c->session = c->ops->open(key, u, &e);
        if (!c->session) {
            pthread_mutex_unlock(&c->lock);
            fr_conn_unref(c);
            *err = e;
            return NULL;
        }
        c->generation++;
        if (fresh) *fresh = 1;
    }
    return c;
}

void *fr_conn_session(const fr_conn *c) { return c->session; }

unsigned fr_conn_generation(const fr_conn *c) { return c->generation; }

void fr_conn_unlock(fr_conn *c, int broken)
{
    if (broken && c->session) {
        c->ops->close(c->session);
        c->session = NULL;
    }
    pthread_mutex_unlock(&c->lock);
}

void fr_conn_put(fr_conn *c, int broken)
{
    fr_conn_unlock(c, broken);
    fr_conn_unref(c);
}

int fr_conn_relock(fr_conn *c, unsigned generation)
{
    pthread_mutex_lock(&c->lock);
    if (c->session && c->generation == generation) return 0;
    pthread_mutex_unlock(&c->lock);
    return -ECONNRESET;
}

void fr_conn_unref(fr_conn *c)
{
    pthread_mutex_lock(&pool_lock);
    c->refs--;
    pthread_mutex_unlock(&pool_lock);
}

void fr_conn_close_all(void)
{
    pthread_mutex_lock(&pool_lock);
    for (int i = 0; i < pool.n;) {
        fr_conn *c = pool.v[i];
        if (c->refs) {
            i++;
            continue;
        }
        if (c->session) c->ops->close(c->session);
        pthread_mutex_destroy(&c->lock);
        free(c->key);
        free(c);
        pool.v[i] = pool.v[--pool.n];
    }
    if (!pool.n) free(pool.v), pool.v = NULL, pool.cap = 0;
    pthread_mutex_unlock(&pool_lock);
}

#include "net.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key;
    fr_cred cred; /* user NULL: nothing known */
    int preloaded, refused;
} entry;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    entry *v;
    int n, cap;
} known;
static const fr_secret_store *store;

static void wipe(char *s)
{
    if (!s) return;
    volatile char *p = s;
    while (*p) *p++ = 0;
    free(s);
}

void fr_cred_free(fr_cred *c)
{
    free(c->user);
    wipe(c->password);
    *c = (fr_cred){0};
}

void fr_creds_set_store(const fr_secret_store *s) { store = s; }

int fr_creds_store_available(void) { return store != NULL; }

/* under lock */
static entry *find(const char *key, int add)
{
    for (int i = 0; i < known.n; i++)
        if (!strcmp(known.v[i].key, key)) return &known.v[i];
    if (!add) return NULL;
    FR_PUSH(known, ((entry){.key = fr_strdup(key)}));
    return &known.v[known.n - 1];
}

int fr_creds_get(const char *key, fr_cred *out)
{
    pthread_mutex_lock(&lock);
    entry *e = find(key, 0);
    int rc = -ENOENT;
    if (e && e->cred.user) {
        out->user = fr_strdup(e->cred.user);
        out->password = fr_strdup(e->cred.password ? e->cred.password : "");
        rc = 0;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

static void set(entry *e, const char *user, const char *password)
{
    fr_cred_free(&e->cred);
    e->cred.user = fr_strdup(user);
    e->cred.password = fr_strdup(password ? password : "");
}

void fr_creds_preload(const char *key)
{
    pthread_mutex_lock(&lock);
    entry *e = find(key, 1);
    int want = !e->preloaded && !e->cred.user && store;
    e->preloaded = 1;
    pthread_mutex_unlock(&lock);
    if (!want) return;
    char *user = NULL, *password = NULL;
    if (!store->load(key, &user, &password) && user) {
        pthread_mutex_lock(&lock);
        e = find(key, 1);
        if (!e->cred.user) set(e, user, password);
        pthread_mutex_unlock(&lock);
    }
    free(user);
    wipe(password);
}

void fr_creds_put(const char *key, const char *user, const char *password, int remember)
{
    pthread_mutex_lock(&lock);
    entry *e = find(key, 1);
    set(e, user, password);
    e->preloaded = 1, e->refused = 0;
    pthread_mutex_unlock(&lock);
    if (!store) return;
    if (remember) store->save(key, user, password ? password : "");
    else store->forget(key);
}

void fr_creds_refuse(const char *key)
{
    pthread_mutex_lock(&lock);
    entry *e = find(key, 1);
    fr_cred_free(&e->cred);
    e->refused = 1;
    pthread_mutex_unlock(&lock);
}

int fr_creds_refused(const char *key)
{
    pthread_mutex_lock(&lock);
    entry *e = find(key, 0);
    int r = e && e->refused;
    pthread_mutex_unlock(&lock);
    return r;
}

void fr_creds_clear(void)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < known.n; i++) {
        free(known.v[i].key);
        fr_cred_free(&known.v[i].cred);
    }
    free(known.v);
    known.v = NULL, known.n = known.cap = 0;
    pthread_mutex_unlock(&lock);
}

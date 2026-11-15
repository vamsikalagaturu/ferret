#include "net.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key, *question;
    void (*accept)(void *data);
    void *data;
    void (*free_data)(void *);
} pending;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    pending *v;
    int n, cap;
} asks;

static void drop(int i)
{
    pending p = asks.v[i];
    asks.v[i] = asks.v[--asks.n];
    free(p.key);
    free(p.question);
    if (p.free_data) p.free_data(p.data);
}

static int find(const char *key)
{
    for (int i = 0; i < asks.n; i++)
        if (!strcmp(asks.v[i].key, key)) return i;
    return -1;
}

void fr_trust_ask(const char *key, const char *question, void (*accept)(void *data), void *data, void (*free_data)(void *))
{
    pthread_mutex_lock(&lock);
    int i = find(key);
    if (i >= 0) drop(i);
    FR_PUSH(asks, ((pending){fr_strdup(key), fr_strdup(question), accept, data, free_data}));
    pthread_mutex_unlock(&lock);
}

char *fr_trust_question(const char *key)
{
    pthread_mutex_lock(&lock);
    int i = find(key);
    char *q = i >= 0 ? fr_strdup(asks.v[i].question) : NULL;
    pthread_mutex_unlock(&lock);
    return q;
}

int fr_trust_answer(const char *key, int accept)
{
    pthread_mutex_lock(&lock);
    int i = find(key);
    if (i < 0) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    pending p = asks.v[i];
    asks.v[i] = asks.v[--asks.n];
    pthread_mutex_unlock(&lock);
    if (accept && p.accept) p.accept(p.data);
    free(p.key);
    free(p.question);
    if (p.free_data) p.free_data(p.data);
    return 1;
}

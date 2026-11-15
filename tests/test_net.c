#define _GNU_SOURCE
#include "check.h"
#include "jobs/async.h"
#include "net/net.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static void parses(const char *uri, const char *scheme, const char *user, const char *host, int port, const char *path)
{
    fr_net_uri u;
    CHECK(fr_net_parse(uri, &u) == 0);
    CHECK_STR(u.scheme, scheme);
    if (user) CHECK_STR(u.user, user);
    else CHECK(u.user == NULL);
    CHECK_STR(u.host, host);
    CHECK(u.port == port);
    CHECK_STR(u.path, path);
    fr_net_uri_free(&u);
}

static void key_is(const char *uri, int share, const char *want)
{
    fr_net_uri u;
    CHECK(fr_net_parse(uri, &u) == 0);
    char *k = share ? fr_net_key_share(&u) : fr_net_key(&u);
    CHECK_STR(k, want);
    free(k);
    fr_net_uri_free(&u);
}

static void normal(const char *typed, const char *want)
{
    char *n = fr_net_normalize(typed);
    if (want) CHECK_STR(n, want);
    else CHECK(n == NULL);
    free(n);
}

static void uris(void)
{
    parses("smb://nas/share/a b", "smb", NULL, "nas", 445, "/share/a b");
    parses("SFTP://Bob@Host.LAN:2222", "sftp", "Bob", "host.lan", 2222, "/");
    parses("sftp://me@x@host/p", "sftp", "me@x", "host", 22, "/p");
    parses("smb://WORK;ann@srv/", "smb", "WORK;ann", "srv", 445, "/");
    parses("sftp://[fe80::1]:23/x", "sftp", NULL, "fe80::1", 23, "/x");
    parses("sftp://[::1]/", "sftp", NULL, "::1", 22, "/");
    fr_net_uri u;
    CHECK(fr_net_parse("/no/scheme", &u) == -EINVAL);
    CHECK(fr_net_parse("smb://", &u) == -EINVAL);
    CHECK(fr_net_parse("smb://host:/x", &u) == -EINVAL);
    CHECK(fr_net_parse("smb://host:99999/x", &u) == -EINVAL);
    CHECK(fr_net_parse("smb://host:4a/x", &u) == -EINVAL);
    CHECK(fr_net_parse("sftp://[::1/x", &u) == -EINVAL);
    CHECK(fr_net_parse("sftp://user@/x", &u) == -EINVAL);

    key_is("sftp://bob@host/home/bob", 0, "sftp://bob@host:22");
    key_is("sftp://host:2222/", 0, "sftp://host:2222");
    key_is("sftp://[::1]/", 0, "sftp://[::1]:22");
    key_is("smb://nas/media/films", 1, "smb://nas:445/media");
    key_is("smb://nas/media", 1, "smb://nas:445/media");
    key_is("smb://nas/", 1, "smb://nas:445/");
    key_is("smb://u@NAS:139/x/y", 0, "smb://u@nas:139");
    char *k = fr_net_key_of("smb://nas/a");
    CHECK_STR(k, "smb://nas:445");
    free(k);
    CHECK(fr_net_key_of("file:") == NULL);

    normal("sftp://Host", "sftp://host/");
    normal("  smb://NAS/Share/ ", "smb://nas/Share");
    normal("sftp://u@h:22/x//", "sftp://u@h/x");
    normal("sftp://h:2200", "sftp://h:2200/");
    normal("smb://ann:secret@nas/x", "smb://ann@nas/x");
    normal("nas/share", NULL);

    char *l = fr_net_label("smb://nas/media/");
    CHECK_STR(l, "nas/media");
    free(l);
    l = fr_net_label("sftp://bob@host/");
    CHECK_STR(l, "bob@host");
    free(l);
}

/* ---- the pool, over a pretend protocol ---- */

static int opens, closes, fail_next;

static void *fake_open(const char *key, const fr_net_uri *u, int *err)
{
    if (fail_next) {
        fail_next = 0;
        *err = -ECONNREFUSED;
        return NULL;
    }
    __atomic_add_fetch(&opens, 1, __ATOMIC_RELAXED);
    return fr_strdup(key);
}

static void fake_close(void *s)
{
    __atomic_add_fetch(&closes, 1, __ATOMIC_RELAXED);
    free(s);
}

static const fr_conn_ops fake = {fake_open, fake_close};

static void *hammer(void *arg)
{
    for (int i = 0; i < 200; i++) {
        int err;
        fr_conn *c = fr_conn_get("x://a:1", NULL, &fake, NULL, &err);
        int *in = arg;
        CHECK(__atomic_add_fetch(in, 1, __ATOMIC_SEQ_CST) == 1); /* one thread at a time on a session */
        __atomic_sub_fetch(in, 1, __ATOMIC_SEQ_CST);
        fr_conn_put(c, 0);
    }
    return NULL;
}

static void pool(void)
{
    int err = 0, fresh;
    fr_conn *a = fr_conn_get("x://a:1", NULL, &fake, &fresh, &err);
    CHECK(a && fresh && opens == 1);
    CHECK_STR(fr_conn_session(a), "x://a:1");
    unsigned gen = fr_conn_generation(a);
    fr_conn_unlock(a, 0); /* a stream keeps it */

    fr_conn *b = fr_conn_get("x://a:1", NULL, &fake, &fresh, &err);
    CHECK(b == a && !fresh && opens == 1);
    fr_conn_put(b, 0);
    fr_conn *other = fr_conn_get("x://b:1", NULL, &fake, &fresh, &err);
    CHECK(other && other != a && fresh && opens == 2);
    fr_conn_put(other, 0);

    CHECK(fr_conn_relock(a, gen) == 0);
    fr_conn_unlock(a, 1); /* the stream's call found it broken */
    CHECK(closes == 1);
    CHECK(fr_conn_relock(a, gen) == -ECONNRESET);
    b = fr_conn_get("x://a:1", NULL, &fake, &fresh, &err);
    CHECK(b == a && fresh && opens == 3 && fr_conn_generation(b) != gen);
    fr_conn_put(b, 0);
    CHECK(fr_conn_relock(a, gen) == -ECONNRESET); /* a new session: the old stream's handle is gone */
    fr_conn_unref(a);

    fail_next = 1;
    CHECK(fr_conn_get("x://c:1", NULL, &fake, &fresh, &err) == NULL && err == -ECONNREFUSED);
    fr_conn *c = fr_conn_get("x://c:1", NULL, &fake, &fresh, &err);
    CHECK(c && fresh);
    fr_conn_put(c, 0);

    int inside = 0;
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, hammer, &inside);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);

    fr_conn_close_all();
    CHECK(opens == closes);
    CHECK(fr_net_broken(-ECONNRESET) && fr_net_broken(-ETIMEDOUT) && !fr_net_broken(-ENOENT) && !fr_net_broken(0));
}

/* ---- passwords ---- */

static char stored_key[64], stored_user[64], stored_pass[64];
static int loads, forgets;

static int store_load(const char *key, char **user, char **password)
{
    loads++;
    if (strcmp(key, stored_key)) return -1;
    *user = fr_strdup(stored_user), *password = fr_strdup(stored_pass);
    return 0;
}

static int store_save(const char *key, const char *user, const char *password)
{
    snprintf(stored_key, sizeof stored_key, "%s", key);
    snprintf(stored_user, sizeof stored_user, "%s", user);
    snprintf(stored_pass, sizeof stored_pass, "%s", password);
    return 0;
}

static int store_forget(const char *key)
{
    forgets++;
    if (!strcmp(key, stored_key)) stored_key[0] = 0;
    return 0;
}

static void creds(void)
{
    fr_cred c = {0};
    CHECK(fr_creds_get("smb://nas:445", &c) == -ENOENT);
    fr_creds_put("smb://nas:445", "ann", "secret", 0);
    CHECK(fr_creds_get("smb://nas:445", &c) == 0);
    CHECK_STR(c.user, "ann");
    CHECK_STR(c.password, "secret");
    fr_cred_free(&c);
    CHECK(c.user == NULL && c.password == NULL);
    fr_creds_refuse("smb://nas:445");
    CHECK(fr_creds_refused("smb://nas:445"));
    CHECK(fr_creds_get("smb://nas:445", &c) == -ENOENT);
    fr_creds_put("smb://nas:445", "ann", "right", 0);
    CHECK(!fr_creds_refused("smb://nas:445"));

    static const fr_secret_store store = {store_load, store_save, store_forget};
    fr_creds_set_store(&store);
    CHECK(fr_creds_store_available());
    fr_creds_put("sftp://h:22", "bob", "pw", 1);
    CHECK_STR(stored_key, "sftp://h:22");
    fr_creds_clear();
    CHECK(fr_creds_get("sftp://h:22", &c) == -ENOENT);
    fr_creds_preload("sftp://h:22");
    fr_creds_preload("sftp://h:22");
    CHECK(loads == 1); /* once per key */
    CHECK(fr_creds_get("sftp://h:22", &c) == 0);
    CHECK_STR(c.user, "bob");
    CHECK_STR(c.password, "pw");
    fr_cred_free(&c);
    fr_creds_put("sftp://h:22", "bob", "pw", 0);
    CHECK(forgets == 1 && !stored_key[0]);
    fr_creds_set_store(NULL);
    fr_creds_clear();
}

/* ---- trust ---- */

static int accepted, freed;
static void on_accept(void *d) { accepted += *(int *)d; }
static void on_free(void *d)
{
    freed++;
    free(d);
}

static int *one(void)
{
    int *p = malloc(sizeof *p);
    *p = 1;
    return p;
}

static void trust(void)
{
    CHECK(fr_trust_question("sftp://h:22") == NULL);
    fr_trust_ask("sftp://h:22", "first", on_accept, one(), on_free);
    fr_trust_ask("sftp://h:22", "second", on_accept, one(), on_free);
    CHECK(freed == 1);
    char *q = fr_trust_question("sftp://h:22");
    CHECK_STR(q, "second");
    free(q);
    CHECK(fr_trust_answer("sftp://h:22", 1) == 1 && accepted == 1 && freed == 2);
    CHECK(fr_trust_answer("sftp://h:22", 1) == 0);
    fr_trust_ask("sftp://g:22", "q", on_accept, one(), on_free);
    CHECK(fr_trust_answer("sftp://g:22", 0) == 1 && accepted == 1 && freed == 3);
}

/* ---- async ---- */

static int worked, done_order[3], ndone;
static void work(void *arg)
{
    usleep((useconds_t)(*(int *)arg * 1000));
    __atomic_add_fetch(&worked, 1, __ATOMIC_RELAXED);
}
static void done(void *arg) { done_order[ndone++] = *(int *)arg; }

static int listed_rc = 1, listed_n = -1;
static char listed_uri[256];
static void on_list(const char *uri, fr_entries *es, int rc, void *ctx)
{
    snprintf(listed_uri, sizeof listed_uri, "%s", uri);
    listed_rc = rc, listed_n = es->n;
    CHECK(ctx == (void *)&listed_rc);
}

static void async(void)
{
    static int slow = 60, fast = 1;
    fr_async_run(work, done, &slow);
    fr_async_run(work, done, &fast);
    fr_async_wait();
    CHECK(worked == 2);
    CHECK(fr_async_poll() == 2);
    CHECK(ndone == 2 && done_order[0] == 1 && done_order[1] == 60); /* in the order they finished */
    CHECK(fr_async_poll() == 0);

    char tmpl[] = "/tmp/ferret-net-XXXXXX";
    CHECK(mkdtemp(tmpl) != NULL);
    char *root = fr_uri_from_path(tmpl), *sub = fr_uri_join(root, "d");
    CHECK(fr_vfs_mkdir(sub) == 0);
    fr_async_list(root, on_list, &listed_rc);
    fr_async_wait();
    fr_async_poll();
    CHECK_STR(listed_uri, root);
    CHECK(listed_rc == 0 && listed_n == 1);
    fr_async_list("nope://x/", on_list, &listed_rc);
    fr_async_wait();
    fr_async_poll();
    CHECK(listed_rc == -EPROTONOSUPPORT);
    CHECK(fr_vfs_remove(sub) == 0 && fr_vfs_remove(root) == 0);
    free(root), free(sub);
}

int main(void)
{
    uris();
    pool();
    creds();
    trust();
    async();
    return CHECK_DONE();
}

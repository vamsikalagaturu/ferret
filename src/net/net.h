#ifndef FR_NET_H
#define FR_NET_H
/* What every network backend shares, whatever its scheme: addresses (scheme://[user@]host[:port]/path), one
 * connection per server kept open between calls (the pool), the passwords given for servers (creds) and the server
 * identities waiting for the user's yes (trust). The VFS backends (vfs_smb.c, vfs_sftp.c) are built on these. */
#include "core/util.h"
#include <errno.h>
#include <stddef.h>

/* The errors only network locations have, as negative errno values like the rest of the VFS: a sign-in is needed or
 * was refused, the server's identity is not known yet (fr_trust_question() has it), or it changed since it was
 * trusted. */
#define FR_EAUTH EKEYREJECTED
#define FR_EUNTRUSTED ENOKEY
#define FR_EIDCHANGED EKEYREVOKED

/* ---- addresses (uri.c) ---- */
typedef struct {
    char scheme[16];
    char *user;       /* NULL when the address names none; "DOMAIN;user" stays as written */
    char *host;       /* without the brackets of an IPv6 literal */
    int port;         /* the scheme's default when the address names none */
    const char *path; /* into the parsed string: "/" at the root, never empty */
} fr_net_uri;

/* 0, or -EINVAL when uri is not scheme://host... (fills nothing then) */
int fr_net_parse(const char *uri, fr_net_uri *out);
void fr_net_uri_free(fr_net_uri *u);
int fr_net_default_port(const char *scheme); /* 0 for a scheme not in the table */
/* the server's name for pools and passwords: "sftp://bob@host:22", "smb://nas:445" (no user named: none) */
char *fr_net_key(const fr_net_uri *u);
/* the same with the path's first segment, for protocols that connect per share: "smb://nas:445/media" */
char *fr_net_key_share(const fr_net_uri *u);
/* the server's part of the key of uri, NULL for an address that does not parse */
char *fr_net_key_of(const char *uri);
/* an address as typed made one the VFS takes: scheme and host in lower case, a "/" path when there was none, no
 * trailing "/" after a folder, no "user:password" password; NULL when it does not parse */
char *fr_net_normalize(const char *typed);
/* "nas/media", "bob@host": what a list of servers shows */
char *fr_net_label(const char *uri);

/* ---- sockets (socket.c) ---- */
/* a blocking TCP socket connected to host:port within timeout_ms; -1 with *err (-EHOSTUNREACH when the name does not
 * resolve) */
int fr_net_connect(const char *host, int port, int timeout_ms, int *err);
/* err means the connection itself is gone (reset, timed out, closed), not that one request failed */
int fr_net_broken(int err);

/* ---- one connection per server (pool.c) ---- */
typedef struct fr_conn fr_conn;
typedef struct {
    /* connects to the server key names (u is the address it came from); the session, or NULL with *err set */
    void *(*open)(const char *key, const fr_net_uri *u, int *err);
    void (*close)(void *session);
} fr_conn_ops;

/* The connection for key, connected (again, when it broke) and locked for this thread, which holds a reference to
 * it; NULL with *err when it could not connect. *fresh (may be NULL) is set when it was connected for this call, so
 * a call that fails on a reused connection can retry once on a new one. Any thread. */
fr_conn *fr_conn_get(const char *key, const fr_net_uri *u, const fr_conn_ops *ops, int *fresh, int *err);
void *fr_conn_session(const fr_conn *c);
/* unlocks; broken: the session is gone (a socket error, a timeout), so it is closed and the next fr_conn_get()
 * connects anew */
void fr_conn_unlock(fr_conn *c, int broken);
void fr_conn_put(fr_conn *c, int broken); /* unlocks and drops the reference */
/* locks it again for a stream that keeps it between calls: 0, or -ECONNRESET when it reconnected or broke since
 * the stream was opened (what the stream had on the server is gone; it stays unlocked then) */
int fr_conn_relock(fr_conn *c, unsigned generation);
unsigned fr_conn_generation(const fr_conn *c);
void fr_conn_unref(fr_conn *c);
/* closes every idle connection (exit, tests) */
void fr_conn_close_all(void);

/* ---- passwords (creds.c) ---- */
typedef struct {
    char *user, *password;
} fr_cred;
void fr_cred_free(fr_cred *c); /* wipes the password */

/* the system's keystore (libsecret, the Android Keystore), called on the window's thread only */
typedef struct {
    int (*load)(const char *key, char **user, char **password); /* 0 found */
    int (*save)(const char *key, const char *user, const char *password);
    int (*forget)(const char *key);
} fr_secret_store;
void fr_creds_set_store(const fr_secret_store *s); /* NULL: none (tests) */
int fr_creds_store_available(void);

/* The password given for server key, from memory: 0 and a copy in *out, else -ENOENT. Any thread. */
int fr_creds_get(const char *key, fr_cred *out);
/* window thread: what the keystore has for key into memory, once per key and run */
void fr_creds_preload(const char *key);
/* window thread: user and password for key from now on; remember: into the keystore too, else out of it */
void fr_creds_put(const char *key, const char *user, const char *password, int remember);
/* the server refused what was given for key: it is dropped from memory (the keystore keeps it until replaced), and
 * fr_creds_refused() says so until the next fr_creds_put(). Any thread. */
void fr_creds_refuse(const char *key);
int fr_creds_refused(const char *key);
void fr_creds_clear(void); /* forgets everything in memory (tests) */

/* ---- server identities waiting for the user (trust.c) ---- */
/* A backend that meets an unknown identity (an SSH host key) asks with a question for the user and what accepting
 * does (writing it to known_hosts), then fails with -FR_EUNTRUSTED; the window shows the question and answers. A
 * newer question for the same key replaces an older one. Any thread. */
void fr_trust_ask(const char *key, const char *question, void (*accept)(void *data), void *data, void (*free_data)(void *));
/* the question waiting for key, malloc'd, NULL none */
char *fr_trust_question(const char *key);
/* runs the accept for key (1) or drops it (0); 0 when nothing waited */
int fr_trust_answer(const char *key, int accept);
#endif

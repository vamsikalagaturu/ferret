#include "net.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* IANA's service names registry; the schemes other backends add go here too. ftps:// is FTP with explicit TLS (AUTH
 * TLS, RFC 4217) on the FTP port: IANA's 990 is implicit TLS, which vfs_ftp.c speaks when the address names 990. */
static const struct {
    const char *scheme;
    int port;
} ports[] = {{"smb", 445}, {"sftp", 22}, {"ftp", 21}, {"ftps", 21}, {"dav", 80}, {"davs", 443}, {"nfs", 2049}};

int fr_net_default_port(const char *scheme)
{
    for (size_t i = 0; i < sizeof ports / sizeof *ports; i++)
        if (!strcmp(ports[i].scheme, scheme)) return ports[i].port;
    return 0;
}

int fr_net_parse(const char *uri, fr_net_uri *out)
{
    const char *sep = strstr(uri, "://");
    size_t sn = sep ? (size_t)(sep - uri) : 0;
    if (!sn || sn >= sizeof out->scheme) return -EINVAL;
    for (size_t i = 0; i < sn; i++)
        if (!isalnum((unsigned char)uri[i]) && uri[i] != '+' && uri[i] != '-' && uri[i] != '.') return -EINVAL;
    const char *auth = sep + 3, *end = strchr(auth, '/');
    if (!end) end = auth + strlen(auth);
    const char *at = NULL;
    for (const char *p = auth; p < end; p++)
        if (*p == '@') at = p;
    const char *host = at ? at + 1 : auth, *host_end, *port = NULL;
    if (*host == '[') {
        host_end = memchr(host, ']', (size_t)(end - host));
        if (!host_end) return -EINVAL;
        host++;
        if (host_end + 1 < end) {
            if (host_end[1] != ':') return -EINVAL;
            port = host_end + 2;
        }
    } else {
        host_end = memchr(host, ':', (size_t)(end - host));
        if (host_end) port = host_end + 1;
        else host_end = end;
    }
    if (host_end == host) return -EINVAL;
    int portn = 0;
    if (port) {
        if (port == end) return -EINVAL;
        for (const char *p = port; p < end; p++) {
            if (!isdigit((unsigned char)*p)) return -EINVAL;
            portn = portn * 10 + (*p - '0');
            if (portn > 65535) return -EINVAL;
        }
        if (!portn) return -EINVAL;
    }
    *out = (fr_net_uri){0};
    for (size_t i = 0; i < sn; i++) out->scheme[i] = (char)tolower((unsigned char)uri[i]);
    out->user = at && at > auth ? fr_strndup(auth, (size_t)(at - auth)) : NULL;
    out->host = fr_strndup(host, (size_t)(host_end - host));
    for (char *h = out->host; *h; h++) *h = (char)tolower((unsigned char)*h);
    out->port = portn ? portn : fr_net_default_port(out->scheme);
    out->path = *end ? end : "/";
    return 0;
}

void fr_net_uri_free(fr_net_uri *u)
{
    free(u->user);
    free(u->host);
    *u = (fr_net_uri){0};
}

static char *authority(const fr_net_uri *u, int with_port)
{
    int v6 = strchr(u->host, ':') != NULL;
    char port[16] = "";
    if (with_port) snprintf(port, sizeof port, ":%d", u->port);
    return fr_format("%s://%s%s%s%s%s%s", u->scheme, u->user ? u->user : "", u->user ? "@" : "", v6 ? "[" : "", u->host,
                     v6 ? "]" : "", port);
}

char *fr_net_key(const fr_net_uri *u) { return authority(u, 1); }

char *fr_net_key_share(const fr_net_uri *u)
{
    const char *seg = u->path + 1, *end = strchr(seg, '/');
    char *key = authority(u, 1), *r = fr_format("%s/%.*s", key, (int)(end ? end - seg : (long)strlen(seg)), seg);
    free(key);
    return r;
}

char *fr_net_key_of(const char *uri)
{
    fr_net_uri u;
    if (fr_net_parse(uri, &u)) return NULL;
    char *k = fr_net_key(&u);
    fr_net_uri_free(&u);
    return k;
}

char *fr_net_normalize(const char *typed)
{
    while (isspace((unsigned char)*typed)) typed++;
    fr_net_uri u;
    if (fr_net_parse(typed, &u)) return NULL;
    char *password = u.user ? strchr(u.user, ':') : NULL;
    if (password) *password = 0;
    char *base = authority(&u, u.port != fr_net_default_port(u.scheme));
    size_t n = strlen(u.path);
    while (n > 1 && (u.path[n - 1] == '/' || isspace((unsigned char)u.path[n - 1]))) n--;
    char *r = fr_format("%s%.*s", base, (int)n, u.path);
    free(base);
    fr_net_uri_free(&u);
    return r;
}

char *fr_net_label(const char *uri)
{
    fr_net_uri u;
    if (fr_net_parse(uri, &u)) return fr_strdup(uri);
    size_t n = strlen(u.path);
    while (n > 1 && u.path[n - 1] == '/') n--;
    char *r = fr_format("%s%s%s%.*s", u.user ? u.user : "", u.user ? "@" : "", u.host, n > 1 ? (int)n : 0, u.path);
    fr_net_uri_free(&u);
    return r;
}

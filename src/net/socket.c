#define _GNU_SOURCE
#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int connect_one(const struct addrinfo *a, int timeout_ms, int *err)
{
    int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
    if (fd < 0) return *err = -errno, -1;
    if (connect(fd, a->ai_addr, a->ai_addrlen) && errno != EINPROGRESS) {
        *err = -errno;
        close(fd);
        return -1;
    }
    struct pollfd p = {fd, POLLOUT, 0};
    int r;
    do r = poll(&p, 1, timeout_ms);
    while (r < 0 && errno == EINTR);
    int soerr = 0;
    socklen_t len = sizeof soerr;
    if (r <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) || soerr) {
        *err = r == 0 ? -ETIMEDOUT : soerr ? -soerr : -errno;
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    return fd;
}

int fr_net_connect(const char *host, int port, int timeout_ms, int *err)
{
    char service[16];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *list = NULL;
    if (getaddrinfo(host, service, &hints, &list)) return *err = -EHOSTUNREACH, -1;
    int fd = -1;
    *err = -EHOSTUNREACH;
    for (struct addrinfo *a = list; a && fd < 0; a = a->ai_next) fd = connect_one(a, timeout_ms, err);
    freeaddrinfo(list);
    return fd;
}

int fr_net_broken(int err)
{
    switch (-err) {
    case ECONNRESET:
    case ECONNABORTED:
    case ETIMEDOUT:
    case ENOTCONN:
    case EPIPE:
    case ESHUTDOWN: return 1;
    default: return 0;
    }
}

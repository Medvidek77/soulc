#define _POSIX_C_SOURCE 200112L
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>

int net_connect(const char *host, const char *port) {
    struct addrinfo hints, *res, *p;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break; // Success
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

void net_close(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

int net_read_exact(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = recv(fd, p, left, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return -1;
        }
        p += n;
        left -= n;
    }
    return 0;
}

int net_write_exact(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        p += n;
        left -= n;
    }
    return 0;
}

int net_wait(int fd, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret > 0) {
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
        if (pfd.revents & POLLIN) return 1;
    }
    if (ret == 0) return 0;
    if (errno == EINTR) return 0;
    return -1;
}

#ifndef NET_H
#define NET_H

#include <stddef.h>

int net_connect(const char *host, const char *port);
void net_close(int fd);

/* Blocking read of exact `len` bytes. Returns 0 on success, -1 on error/EOF */
int net_read_exact(int fd, void *buf, size_t len);

/* Blocking write of exact `len` bytes. Returns 0 on success, -1 on error */
int net_write_exact(int fd, const void *buf, size_t len);

/* Wait for data on socket. timeout_ms: <0 block indefinitely, 0 return immediately, >0 wait ms.
 * Returns 1 if data available, 0 if timeout, -1 on error. */
int net_wait(int fd, int timeout_ms);

/* Create a listening TCP socket on a specific port. Returns fd or -1 on error. */
int net_listen(const char *port);

/* Accept incoming connection on listening socket. Returns new fd or -1 on error. */
int net_accept(int listen_fd);

#endif

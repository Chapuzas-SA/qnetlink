#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void handle_stop_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

int platform_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0) return -1;
    stop_requested = 0;
    return 0;
}
void platform_cleanup(void) { }
int platform_stop_requested(void) { return stop_requested != 0; }
int platform_poll_interrupted(void) { return errno == EINTR; }

int64_t platform_clock_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t platform_clock_ms(void) { return platform_clock_ns() / 1000000LL; }

void platform_sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}

int platform_tcp_connect_start(const char *host, uint16_t port,
                               slirp_os_socket *out, int *connected)
{
    struct addrinfo hints, *res = NULL, *it;
    char service[16];
    int rc = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &res) != 0) return -1;
    for (it = res; it; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) continue;
        if (set_nonblocking(fd) != 0) { close(fd); continue; }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            *out = fd; *connected = 1; rc = 0; break;
        }
        if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
            *out = fd; *connected = 0; rc = 0; break;
        }
        close(fd);
    }
    freeaddrinfo(res);
    return rc;
}

int platform_tcp_connect_finish(slirp_os_socket fd)
{
    int err = 0; socklen_t len = sizeof(err);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0 ? 0 : -1;
}

int platform_tcp_listen_bind_ipv4(const char *address, uint16_t port,
                                  slirp_os_socket *out, int *error_code)
{
    struct sockaddr_in addr;
    int fd, one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) goto fail;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0)
        goto fail_close;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &addr.sin_addr) != 1) {
        errno = EINVAL;
        goto fail_close;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        goto fail_close;
    *out = fd;
    return 0;

fail_close:
    *error_code = errno;
    close(fd);
    return -1;
fail:
    *error_code = errno;
    return -1;
}

int platform_socket_set_nodelay(slirp_os_socket fd)
{
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void platform_socket_close(slirp_os_socket fd) { if (fd != SLIRP_INVALID_SOCKET) close(fd); }
long platform_socket_recv(slirp_os_socket fd, void *buf, size_t len) { return (long)recv(fd, buf, len, 0); }
long platform_socket_send(slirp_os_socket fd, const void *buf, size_t len) { return (long)send(fd, buf, len, MSG_NOSIGNAL); }
int platform_socket_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }

int platform_poll(PlatformPollFd *fds, size_t count, int timeout_ms)
{
    struct pollfd *pfds;
    size_t i;
    int rc;
    if (count > (size_t)INT_MAX) return -1;
    pfds = count ? calloc(count, sizeof(*pfds)) : NULL;
    if (count && !pfds) return -1;
    for (i = 0; i < count; ++i) {
        pfds[i].fd = fds[i].fd;
        if (fds[i].events & PLATFORM_POLL_IN) pfds[i].events |= POLLIN;
        if (fds[i].events & PLATFORM_POLL_OUT) pfds[i].events |= POLLOUT;
    }
    rc = poll(pfds, (nfds_t)count, timeout_ms);
    if (rc >= 0) for (i = 0; i < count; ++i) {
        fds[i].revents = 0;
        if (pfds[i].revents & (POLLIN | POLLPRI)) fds[i].revents |= PLATFORM_POLL_IN;
        if (pfds[i].revents & POLLOUT) fds[i].revents |= PLATFORM_POLL_OUT;
        if (pfds[i].revents & POLLERR) fds[i].revents |= PLATFORM_POLL_ERR;
        if (pfds[i].revents & (POLLHUP | POLLNVAL)) fds[i].revents |= PLATFORM_POLL_HUP;
    }
    free(pfds);
    return rc;
}

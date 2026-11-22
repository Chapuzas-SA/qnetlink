#ifndef QNETLINK_PLATFORM_H
#define QNETLINK_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <libslirp.h>

#define PLATFORM_POLL_IN  0x01
#define PLATFORM_POLL_OUT 0x02
#define PLATFORM_POLL_ERR 0x04
#define PLATFORM_POLL_HUP 0x08

typedef struct PlatformPollFd {
    slirp_os_socket fd;
    int events;
    int revents;
} PlatformPollFd;

int platform_init(void);
void platform_cleanup(void);
int platform_stop_requested(void);
int platform_poll_interrupted(void);
int64_t platform_clock_ns(void);
int64_t platform_clock_ms(void);
void platform_sleep_ms(unsigned ms);
int platform_tcp_connect_start(const char *host, uint16_t port,
                               slirp_os_socket *fd, int *connected);
int platform_tcp_connect_finish(slirp_os_socket fd);
int platform_tcp_listen_bind_ipv4(const char *address, uint16_t port,
                                  slirp_os_socket *fd, int *error_code);
int platform_socket_set_nodelay(slirp_os_socket fd);
void platform_socket_close(slirp_os_socket fd);
long platform_socket_recv(slirp_os_socket fd, void *buf, size_t len);
long platform_socket_send(slirp_os_socket fd, const void *buf, size_t len);
int platform_socket_would_block(void);
int platform_poll(PlatformPollFd *fds, size_t count, int timeout_ms);

#endif

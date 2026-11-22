#include "platform.h"

#ifdef _WIN32
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mstcpip.h>

static int last_error;
static volatile LONG stop_requested;

static BOOL WINAPI console_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        InterlockedExchange(&stop_requested, 1);
        return TRUE;
    default:
        return FALSE;
    }
}

int platform_init(void)
{
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return -1;
    InterlockedExchange(&stop_requested, 0);
    if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
        WSACleanup();
        return -1;
    }
    return 0;
}
void platform_cleanup(void)
{
    SetConsoleCtrlHandler(console_handler, FALSE);
    WSACleanup();
}
int platform_stop_requested(void) { return InterlockedCompareExchange(&stop_requested, 0, 0) != 0; }
int platform_poll_interrupted(void) { return last_error == WSAEINTR; }

int64_t platform_clock_ns(void)
{
    LARGE_INTEGER f, c;
    if (!QueryPerformanceFrequency(&f) || !QueryPerformanceCounter(&c)) return 0;
    return (int64_t)((c.QuadPart / f.QuadPart) * 1000000000LL +
                     ((c.QuadPart % f.QuadPart) * 1000000000LL) / f.QuadPart);
}
int64_t platform_clock_ms(void) { return platform_clock_ns() / 1000000LL; }
void platform_sleep_ms(unsigned ms) { Sleep(ms); }

int platform_tcp_connect_start(const char *host, uint16_t port,
                               slirp_os_socket *out, int *connected)
{
    struct addrinfo hints, *res = NULL, *it;
    char service[16];
    int rc = -1;
    u_long nonblock = 1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &res) != 0) return -1;
    for (it = res; it; it = it->ai_next) {
        SOCKET fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == INVALID_SOCKET) continue;
        if (ioctlsocket(fd, FIONBIO, &nonblock) != 0) { closesocket(fd); continue; }
        if (connect(fd, it->ai_addr, (int)it->ai_addrlen) == 0) {
            *out = fd; *connected = 1; rc = 0; break;
        }
        last_error = WSAGetLastError();
        if (last_error == WSAEWOULDBLOCK || last_error == WSAEINPROGRESS || last_error == WSAEINVAL) {
            *out = fd; *connected = 0; rc = 0; break;
        }
        closesocket(fd);
    }
    freeaddrinfo(res);
    return rc;
}

int platform_tcp_connect_finish(slirp_os_socket fd)
{
    int err = 0; int len = sizeof(err);
    return getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 && err == 0 ? 0 : -1;
}

int platform_tcp_listen_bind_ipv4(const char *address, uint16_t port,
                                  slirp_os_socket *out, int *error_code)
{
    struct sockaddr_in addr;
    SOCKET fd;
    BOOL exclusive = TRUE;

    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) goto fail;
    if (setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&exclusive, sizeof(exclusive)) == SOCKET_ERROR)
        goto fail_close;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (InetPtonA(AF_INET, address, &addr.sin_addr) != 1) {
        *error_code = WSAEINVAL;
        goto fail_close;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
        goto fail_close;
    *out = fd;
    return 0;

fail_close:
    if (*error_code != WSAEINVAL) *error_code = WSAGetLastError();
    last_error = *error_code;
    closesocket(fd);
    return -1;
fail:
    last_error = WSAGetLastError();
    *error_code = last_error;
    return -1;
}

int platform_socket_set_nodelay(slirp_os_socket fd)
{
    BOOL one = TRUE;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
}
void platform_socket_close(slirp_os_socket fd) { if (fd != INVALID_SOCKET) closesocket(fd); }
long platform_socket_recv(slirp_os_socket fd, void *buf, size_t len)
{
    int n = recv(fd, (char *)buf, len > INT_MAX ? INT_MAX : (int)len, 0);
    if (n == SOCKET_ERROR) last_error = WSAGetLastError();
    return n;
}
long platform_socket_send(slirp_os_socket fd, const void *buf, size_t len)
{
    int n = send(fd, (const char *)buf, len > INT_MAX ? INT_MAX : (int)len, 0);
    if (n == SOCKET_ERROR) last_error = WSAGetLastError();
    return n;
}
int platform_socket_would_block(void) { return last_error == WSAEWOULDBLOCK; }

int platform_poll(PlatformPollFd *fds, size_t count, int timeout_ms)
{
    WSAPOLLFD *pfds;
    size_t i;
    int rc;
    if (count > INT_MAX) return -1;
    if (count == 0) {
        Sleep(timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
        return 0;
    }
    pfds = calloc(count, sizeof(*pfds));
    if (count && !pfds) return -1;
    for (i = 0; i < count; ++i) {
        pfds[i].fd = fds[i].fd;
        if (fds[i].events & PLATFORM_POLL_IN) pfds[i].events |= POLLRDNORM;
        if (fds[i].events & PLATFORM_POLL_OUT) pfds[i].events |= POLLWRNORM;
    }
    rc = WSAPoll(pfds, (ULONG)count, timeout_ms);
    if (rc == SOCKET_ERROR) last_error = WSAGetLastError();
    if (rc >= 0) for (i = 0; i < count; ++i) {
        fds[i].revents = 0;
        if (pfds[i].revents & (POLLRDNORM | POLLRDBAND)) fds[i].revents |= PLATFORM_POLL_IN;
        if (pfds[i].revents & (POLLWRNORM | POLLWRBAND)) fds[i].revents |= PLATFORM_POLL_OUT;
        if (pfds[i].revents & POLLERR) fds[i].revents |= PLATFORM_POLL_ERR;
        if (pfds[i].revents & (POLLHUP | POLLNVAL)) fds[i].revents |= PLATFORM_POLL_HUP;
    }
    free(pfds);
    return rc;
}
#endif

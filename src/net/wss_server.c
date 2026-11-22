#include "wss_server.h"
#include "app/logger.h"
#include "platform.h"
#include "protocol/stream.h"
#include "protocol/wss_message.h"
#include <libwebsockets.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVER_TX_LIMIT (4U * 1024U * 1024U)

typedef enum ServerTcpState {
    SERVER_TCP_DOWN,
    SERVER_TCP_CONNECTING,
    SERVER_TCP_UP
} ServerTcpState;

typedef struct ServerFrame {
    unsigned char *data;
    size_t len;
    size_t sent;
    struct ServerFrame *next;
} ServerFrame;

typedef struct ServerWssPoll {
    lws_sockfd_type fd;
    int events;
    size_t external_index;
} ServerWssPoll;

typedef struct WssServer {
    const AppConfig *config;
    struct lws_context *context;
    struct lws *wsi;
    WssMessageReceiver ws_rx;
    StreamReceiver tcp_rx;
    slirp_os_socket tcp_sock;
    ServerTcpState tcp_state;
    int64_t reconnect_at;
    int qemu_outage_reported;
    ServerFrame *to_tcp_head, *to_tcp_tail;
    ServerFrame *to_ws_head, *to_ws_tail;
    size_t to_tcp_bytes, to_ws_bytes;
    int to_tcp_overflow_reported, to_ws_overflow_reported;
    size_t tcp_poll_index;
    int has_tcp_poll;
    ServerWssPoll *wss_polls;
    size_t wss_poll_count, wss_poll_capacity;
    size_t wss_poll_base, wss_fill_count;
} WssServer;

static int server_callback(struct lws *wsi, enum lws_callback_reasons reason,
                           void *user, void *in, size_t len);

static const struct lws_protocols server_protocols[] = {
    { "qemu-stream", server_callback, 0, STREAM_MAX_PACKET_SIZE + STREAM_HEADER_SIZE, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

static long server_poll_find(const WssServer *s, lws_sockfd_type fd)
{
    size_t i;
    for (i = 0; i < s->wss_poll_count; i++)
        if (s->wss_polls[i].fd == fd) return (long)i;
    return -1;
}

static int server_poll_add(WssServer *s, const struct lws_pollargs *pa)
{
    long found = server_poll_find(s, pa->fd);
    size_t cap;
    ServerWssPoll *p;
    if (found >= 0) {
        s->wss_polls[(size_t)found].events = pa->events;
        return 0;
    }
    if (s->wss_poll_count == s->wss_poll_capacity) {
        cap = s->wss_poll_capacity ? s->wss_poll_capacity * 2U : 8U;
        if (cap < s->wss_poll_capacity || cap > SIZE_MAX / sizeof(*p)) return -1;
        p = realloc(s->wss_polls, cap * sizeof(*p));
        if (!p) return -1;
        s->wss_polls = p;
        s->wss_poll_capacity = cap;
    }
    s->wss_polls[s->wss_poll_count].fd = pa->fd;
    s->wss_polls[s->wss_poll_count].events = pa->events;
    s->wss_polls[s->wss_poll_count].external_index = SIZE_MAX;
    s->wss_poll_count++;
    return 0;
}

static void server_poll_del(WssServer *s, lws_sockfd_type fd)
{
    long found = server_poll_find(s, fd);
    size_t i;
    if (found < 0) return;
    i = (size_t)found;
    s->wss_poll_count--;
    if (i != s->wss_poll_count) s->wss_polls[i] = s->wss_polls[s->wss_poll_count];
}

static void server_poll_change(WssServer *s, const struct lws_pollargs *pa)
{
    long found = server_poll_find(s, pa->fd);
    if (found >= 0) s->wss_polls[(size_t)found].events = pa->events;
}

static int server_poll_reserve(PlatformPollFd **fds, size_t *capacity, size_t need)
{
    PlatformPollFd *p;
    size_t cap = *capacity ? *capacity : 16U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) return -1;
        cap *= 2U;
    }
    if (cap == *capacity) return 0;
    p = realloc(*fds, cap * sizeof(*p));
    if (!p) return -1;
    *fds = p;
    *capacity = cap;
    return 0;
}

static int server_to_platform_events(int events)
{
    int out = 0;
    if (events & LWS_POLLIN) out |= PLATFORM_POLL_IN;
    if (events & LWS_POLLOUT) out |= PLATFORM_POLL_OUT;
    return out;
}

static short server_to_lws_revents(int events)
{
    short out = 0;
    if (events & PLATFORM_POLL_IN) out |= (short)LWS_POLLIN;
    if (events & PLATFORM_POLL_OUT) out |= (short)LWS_POLLOUT;
    if (events & (PLATFORM_POLL_ERR | PLATFORM_POLL_HUP)) out |= (short)LWS_POLLHUP;
    return out;
}

static void queue_clear(ServerFrame **head, ServerFrame **tail, size_t *bytes)
{
    ServerFrame *f, *n;
    for (f = *head; f; f = n) {
        n = f->next;
        free(f->data);
        free(f);
    }
    *head = *tail = NULL;
    *bytes = 0;
}

static int queue_raw(ServerFrame **head, ServerFrame **tail, size_t *bytes,
                     const void *data, size_t len, size_t prefix)
{
    ServerFrame *f;
    if (len > SERVER_TX_LIMIT || *bytes > SERVER_TX_LIMIT - len) return -1;
    f = calloc(1, sizeof(*f));
    if (!f) return -1;
    f->data = malloc(prefix + len);
    if (!f->data) {
        free(f);
        return -1;
    }
    memset(f->data, 0, prefix);
    memcpy(f->data + prefix, data, len);
    f->len = prefix + len;
    f->sent = prefix;
    if (*tail) (*tail)->next = f;
    else *head = f;
    *tail = f;
    *bytes += len;
    return 0;
}

static void disconnect_tcp(WssServer *s)
{
    if (s->tcp_sock != SLIRP_INVALID_SOCKET)
        platform_socket_close(s->tcp_sock);
    s->tcp_sock = SLIRP_INVALID_SOCKET;
    s->tcp_state = SERVER_TCP_DOWN;
    stream_receiver_init(&s->tcp_rx);
    queue_clear(&s->to_tcp_head, &s->to_tcp_tail, &s->to_tcp_bytes);
    s->reconnect_at = platform_clock_ms() + s->config->reconnect_ms;
    if (!s->qemu_outage_reported) {
        logger_log(LOG_LEVEL_WARN, "transport", "qemu_unavailable", "peer=%s:%u retry_delay_ms=%u", s->config->stream_host, (unsigned)s->config->stream_port, s->config->reconnect_ms);
        s->qemu_outage_reported = 1;
    }
}

static int start_tcp_connect(WssServer *s)
{
    int connected = 0;
    if (!s->wsi) return 0;
    if (platform_tcp_connect_start(s->config->stream_host, s->config->stream_port,
                                   &s->tcp_sock, &connected) != 0) {
        s->reconnect_at = platform_clock_ms() + s->config->reconnect_ms;
        if (!s->qemu_outage_reported) {
            logger_log(LOG_LEVEL_WARN, "transport", "qemu_unavailable", "peer=%s:%u retry_delay_ms=%u", s->config->stream_host, (unsigned)s->config->stream_port, s->config->reconnect_ms);
            s->qemu_outage_reported = 1;
        }
        return -1;
    }
    s->tcp_state = connected ? SERVER_TCP_UP : SERVER_TCP_CONNECTING;
    if (connected) {
        s->qemu_outage_reported = 0;
        platform_socket_set_nodelay(s->tcp_sock);
        logger_log(LOG_LEVEL_INFO, "transport", "qemu_connected", "peer=%s:%u", s->config->stream_host, (unsigned)s->config->stream_port);
    } else {
        logger_log(LOG_LEVEL_DEBUG, "transport", "qemu_connecting", "peer=%s:%u", s->config->stream_host, (unsigned)s->config->stream_port);
    }
    return 0;
}

static int flush_tcp(WssServer *s)
{
    while (s->to_tcp_head) {
        ServerFrame *f = s->to_tcp_head;
        long n = platform_socket_send(s->tcp_sock, f->data + f->sent, f->len - f->sent);
        if (n < 0) {
            if (platform_socket_would_block()) return 0;
            return -1;
        }
        if (n == 0) return -1;
        f->sent += (size_t)n;
        if (f->sent < f->len) return 0;
        s->to_tcp_bytes -= f->len;
        if (s->to_tcp_bytes < SERVER_TX_LIMIT / 2U) s->to_tcp_overflow_reported = 0;
        s->to_tcp_head = f->next;
        if (!s->to_tcp_head) s->to_tcp_tail = NULL;
        free(f->data);
        free(f);
    }
    return 0;
}

static void tcp_packet_to_ws(const uint8_t *packet, size_t len, void *opaque)
{
    WssServer *s = opaque;
    uint8_t frame[STREAM_HEADER_SIZE + STREAM_MAX_PACKET_SIZE];
    size_t frame_len = 0;
    if (!s->wsi) return;
    if (stream_build_frame(packet, len, frame, sizeof(frame), &frame_len) != 0) {
        logger_log(LOG_LEVEL_ERROR, "server", "frame_build_failed", "failed to build QEMU stream frame for WSS");
        return;
    }
    if (queue_raw(&s->to_ws_head, &s->to_ws_tail, &s->to_ws_bytes,
                  frame, frame_len, LWS_PRE) != 0) {
        if (!s->to_ws_overflow_reported) {
            logger_log(LOG_LEVEL_WARN, "server", "frame_dropped", "QEMU frames dropped for WSS because the queue is full");
            s->to_ws_overflow_reported = 1;
        }
        return;
    }
    lws_callback_on_writable(s->wsi);
}

static int read_tcp(WssServer *s)
{
    uint8_t buf[16384];
    for (;;) {
        long n = platform_socket_recv(s->tcp_sock, buf, sizeof(buf));
        if (n > 0) {
            if (stream_receiver_feed(&s->tcp_rx, buf, (size_t)n, tcp_packet_to_ws, s) != 0)
                return -1;
            continue;
        }
        if (n == 0) return -1;
        if (platform_socket_would_block()) return 0;
        return -1;
    }
}

static int flush_ws(WssServer *s, struct lws *wsi)
{
    ServerFrame *f = s->to_ws_head;
    int n;

    if (!f) return 0;
    n = lws_write(wsi, f->data + LWS_PRE, f->len - LWS_PRE, LWS_WRITE_BINARY);
    if (n < (int)(f->len - LWS_PRE)) return -1;
    s->to_ws_bytes -= f->len - LWS_PRE;
    if (s->to_ws_bytes < SERVER_TX_LIMIT / 2U) s->to_ws_overflow_reported = 0;
    s->to_ws_head = f->next;
    if (!s->to_ws_head) s->to_ws_tail = NULL;
    free(f->data);
    free(f);
    if (s->to_ws_head) lws_callback_on_writable(wsi);
    return 0;
}

static void ws_packet_to_tcp(const uint8_t *packet, size_t len, void *opaque)
{
    WssServer *s = opaque;
    uint8_t frame[STREAM_HEADER_SIZE + STREAM_MAX_PACKET_SIZE];
    size_t frame_len = 0;
    if (stream_build_frame(packet, len, frame, sizeof(frame), &frame_len) != 0) {
        logger_log(LOG_LEVEL_WARN, "server", "invalid_wss_packet", "received invalid WSS QEMU packet");
        return;
    }
    if (queue_raw(&s->to_tcp_head, &s->to_tcp_tail, &s->to_tcp_bytes,
                  frame, frame_len, 0) != 0) {
        if (!s->to_tcp_overflow_reported) {
            logger_log(LOG_LEVEL_WARN, "server", "frame_dropped", "WSS QEMU packets dropped because the queue is full or QEMU is unavailable");
            s->to_tcp_overflow_reported = 1;
        }
    }
}

static WssServer *from_context(struct lws *wsi)
{
    return (WssServer *)lws_context_user(lws_get_context(wsi));
}

static int server_callback(struct lws *wsi, enum lws_callback_reasons reason,
                           void *user, void *in, size_t len)
{
    WssServer *s = from_context(wsi);
    (void)user;
    if (!s) return 0;
    switch (reason) {
    case LWS_CALLBACK_HTTP:
        logger_log(LOG_LEVEL_WARN, "server", "http_request_rejected", "rejected non-WebSocket HTTP request");
        if (lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL) ||
            lws_http_transaction_completed(wsi))
            return -1;
        return 0;
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION: {
        char path[256];
        if (lws_hdr_copy(wsi, path, sizeof(path), WSI_TOKEN_GET_URI) < 0 ||
            strcmp(path, s->config->ws_path) != 0) {
            logger_log(LOG_LEVEL_WARN, "server", "wss_path_rejected", "rejected WebSocket path");
            lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, NULL);
            return -1;
        }
        break;
    }
    case LWS_CALLBACK_ESTABLISHED:
        if (s->wsi && s->wsi != wsi) return -1;
        s->wsi = wsi;
        if (platform_socket_set_nodelay((slirp_os_socket)lws_get_socket_fd(wsi)) != 0)
            logger_log(LOG_LEVEL_WARN, "transport", "tcp_nodelay_failed", "could not disable Nagle on WSS server socket");
        wss_message_receiver_init(&s->ws_rx);
        stream_receiver_init(&s->tcp_rx);
        s->reconnect_at = platform_clock_ms();
        logger_log(LOG_LEVEL_INFO, "server", "wss_client_connected", "WSS client connected");
        break;
    case LWS_CALLBACK_RECEIVE:
        if (!lws_frame_is_binary(wsi)) return -1;
        if (wss_message_receiver_feed(&s->ws_rx, in, len, lws_is_final_fragment(wsi),
                                      ws_packet_to_tcp, s) != 0)
            return -1;
        break;
    case LWS_CALLBACK_SERVER_WRITEABLE:
        if (flush_ws(s, wsi) != 0) return -1;
        break;
    case LWS_CALLBACK_CLOSED:
        if (s->wsi == wsi) {
            logger_log(LOG_LEVEL_WARN, "server", "wss_client_disconnected", "WSS client disconnected");
            s->wsi = NULL;
            queue_clear(&s->to_ws_head, &s->to_ws_tail, &s->to_ws_bytes);
            disconnect_tcp(s);
        }
        break;
    case LWS_CALLBACK_ADD_POLL_FD:
        if (server_poll_add(s, (const struct lws_pollargs *)in) != 0) return -1;
        break;
    case LWS_CALLBACK_DEL_POLL_FD:
        server_poll_del(s, ((const struct lws_pollargs *)in)->fd);
        break;
    case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
        server_poll_change(s, (const struct lws_pollargs *)in);
        break;
    default:
        break;
    }
    return 0;
}

static int fill_poll(WssServer *s, PlatformPollFd **fds, size_t *count,
                     size_t *capacity, uint32_t *timeout)
{
    int64_t now = platform_clock_ms();
    size_t i;

    if (s->wsi && s->tcp_state == SERVER_TCP_DOWN && now >= s->reconnect_at)
        (void)start_tcp_connect(s);
    s->wss_poll_base = *count;
    s->wss_fill_count = 0;
    if (server_poll_reserve(fds, capacity, *count + s->wss_poll_count +
                            (s->tcp_state != SERVER_TCP_DOWN ? 1U : 0U)) != 0)
        return -1;
    for (i = 0; i < s->wss_poll_count; i++) {
        ServerWssPoll *p = &s->wss_polls[i];
        p->external_index = *count;
        (*fds)[*count].fd = (slirp_os_socket)p->fd;
        (*fds)[*count].events = server_to_platform_events(p->events);
        (*fds)[*count].revents = 0;
        (*count)++;
        s->wss_fill_count++;
    }
    s->has_tcp_poll = 0;
    if (s->tcp_state != SERVER_TCP_DOWN) {
        PlatformPollFd *fd = &(*fds)[*count];
        s->tcp_poll_index = *count;
        s->has_tcp_poll = 1;
        fd->fd = s->tcp_sock;
        fd->events = PLATFORM_POLL_IN;
        if (s->tcp_state == SERVER_TCP_CONNECTING || s->to_tcp_head)
            fd->events |= PLATFORM_POLL_OUT;
        fd->revents = 0;
        (*count)++;
    } else if (s->wsi) {
        int64_t delay = s->reconnect_at - now;
        uint32_t wait = delay <= 0 ? 0U : delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
        if (wait < *timeout) *timeout = wait;
    }
    {
        int input = *timeout > INT_MAX ? INT_MAX : (int)*timeout;
        int adjusted = lws_service_adjust_timeout(s->context, input, 0);
        if (adjusted <= 0) *timeout = 0;
        else if ((uint32_t)adjusted < *timeout) *timeout = (uint32_t)adjusted;
    }
    return 0;
}

static int dispatch_wss(WssServer *s, PlatformPollFd *fds)
{
    ServerWssPoll *ready;
    size_t ready_count = 0, i;
    int rc = 0;
    ready = s->wss_fill_count ? malloc(s->wss_fill_count * sizeof(*ready)) : NULL;
    if (s->wss_fill_count && !ready) return -1;
    for (i = 0; i < s->wss_fill_count; i++) {
        size_t x = s->wss_poll_base + i;
        if (fds[x].revents) {
            ready[ready_count] = s->wss_polls[i];
            ready[ready_count].external_index = x;
            ready_count++;
        }
    }
    for (i = 0; i < ready_count; i++) {
        struct lws_pollfd pfd;
        ServerWssPoll *p = &ready[i];
        if (server_poll_find(s, p->fd) < 0) continue;
        pfd.fd = p->fd;
        pfd.events = p->events;
        pfd.revents = server_to_lws_revents(fds[p->external_index].revents);
        if (lws_service_fd(s->context, &pfd) < 0) {
            rc = -1;
            break;
        }
    }
    free(ready);
    if (rc == 0 && lws_service_adjust_timeout(s->context, 1, 0) == 0 &&
        lws_service_tsi(s->context, -1, 0) < 0)
        rc = -1;
    return rc;
}

static int service_tcp(WssServer *s, PlatformPollFd *fds)
{
    int re;
    if (!s->has_tcp_poll || s->tcp_state == SERVER_TCP_DOWN) return 0;
    re = fds[s->tcp_poll_index].revents;

    if (s->tcp_state == SERVER_TCP_CONNECTING &&
        (re & (PLATFORM_POLL_OUT | PLATFORM_POLL_ERR | PLATFORM_POLL_HUP))) {
        if (platform_tcp_connect_finish(s->tcp_sock) != 0)
            disconnect_tcp(s);
        else {
            s->tcp_state = SERVER_TCP_UP;
            s->qemu_outage_reported = 0;
            platform_socket_set_nodelay(s->tcp_sock);
            logger_log(LOG_LEVEL_INFO, "transport", "qemu_connected", "QEMU stream connected");
        }
    } else if (s->tcp_state == SERVER_TCP_UP) {
        if (re & (PLATFORM_POLL_ERR | PLATFORM_POLL_HUP))
            disconnect_tcp(s);
        else {
            if ((re & PLATFORM_POLL_IN) && read_tcp(s) != 0)
                disconnect_tcp(s);
            if (s->tcp_state == SERVER_TCP_UP &&
                ((re & PLATFORM_POLL_OUT) || s->to_tcp_head) &&
                flush_tcp(s) != 0)
                disconnect_tcp(s);
        }
    }
    return 0;
}

int wss_server_run(const AppConfig *config)
{
    WssServer s;
    struct lws_context_creation_info info;
    slirp_os_socket listen_sock = SLIRP_INVALID_SOCKET;
    int socket_error = 0;
    int exit_code = 1;
    PlatformPollFd *fds = NULL;
    size_t fds_capacity = 0;

    memset(&s, 0, sizeof(s));
    s.config = config;
    s.tcp_sock = SLIRP_INVALID_SOCKET;
    s.reconnect_at = platform_clock_ms();
    wss_message_receiver_init(&s.ws_rx);
    stream_receiver_init(&s.tcp_rx);

    /* LWS iface is an interface name, not an IP address. NULL means all. */
    if (strcmp(config->listen_host, "0.0.0.0") != 0 &&
        platform_tcp_listen_bind_ipv4(config->listen_host, config->listen_port,
                                      &listen_sock, &socket_error) != 0) {
        logger_log(LOG_LEVEL_ERROR, "server", "listen_socket_bind_failed",
                   "failed to bind WSS listener to %s:%u (socket error %d)",
                   config->listen_host, (unsigned)config->listen_port, socket_error);
        goto out;
    }

    memset(&info, 0, sizeof(info));
    info.port = (int)config->listen_port;
    info.vh_listen_sockfd = listen_sock == SLIRP_INVALID_SOCKET ? 0 : listen_sock;
    info.iface = NULL;
    info.protocols = server_protocols;
    info.user = &s;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    if (listen_sock == SLIRP_INVALID_SOCKET)
        info.options |= LWS_SERVER_OPTION_DISABLE_IPV6;
    info.ssl_cert_filepath = config->tls_cert_path;
    info.ssl_private_key_filepath = config->tls_key_path;

    s.context = lws_create_context(&info);
    if (!s.context) {
        logger_log(LOG_LEVEL_ERROR, "server", "wss_listener_init_failed", "failed to initialize WSS listener");
        goto out;
    }

    logger_log(LOG_LEVEL_INFO, "server", "listening", "listen=wss://%s:%u%s qemu_peer=%s:%u listener=%s",
               config->listen_host, (unsigned)config->listen_port, config->ws_path,
               config->stream_host, (unsigned)config->stream_port,
               listen_sock == SLIRP_INVALID_SOCKET ? "lws-owned" : "app-bound");

    for (;;) {
        size_t count = 0;
        uint32_t timeout = 250U;
        int rc;
        if (platform_stop_requested()) {
            exit_code = 0;
            break;
        }
        if (fill_poll(&s, &fds, &count, &fds_capacity, &timeout) != 0) {
            logger_log(LOG_LEVEL_ERROR, "server", "poll_set_failed", "failed to build server poll set");
            break;
        }
        rc = platform_poll(fds, count, (int)timeout);
        if (rc < 0) {
            if (platform_stop_requested()) { exit_code = 0; break; }
            if (platform_poll_interrupted()) continue;
            logger_log(LOG_LEVEL_ERROR, "server", "poll_failed", "server poll failed");
            break;
        }
        if (dispatch_wss(&s, fds) != 0) {
            logger_log(LOG_LEVEL_ERROR, "server", "wss_service_failed", "WSS event dispatch failed");
            break;
        }
        if (service_tcp(&s, fds) != 0) {
            logger_log(LOG_LEVEL_ERROR, "server", "tcp_poll_failed", "TCP poll failed");
            break;
        }
    }

out:
    if (s.context) lws_context_destroy(s.context);
    platform_socket_close(listen_sock);
    if (s.tcp_sock != SLIRP_INVALID_SOCKET) platform_socket_close(s.tcp_sock);
    queue_clear(&s.to_tcp_head, &s.to_tcp_tail, &s.to_tcp_bytes);
    queue_clear(&s.to_ws_head, &s.to_ws_tail, &s.to_ws_bytes);
    free(s.wss_polls);
    free(fds);
    return exit_code;
}

#include "transport_wss.h"
#include "protocol/stream.h"
#include "protocol/wss_message.h"
#include "app/logger.h"
#include "platform.h"
#include <libwebsockets.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WSS_TX_LIMIT (4U * 1024U * 1024U)

typedef struct WssTx {
    unsigned char *alloc;
    size_t len;
    struct WssTx *next;
} WssTx;

typedef struct WssPoll {
    lws_sockfd_type fd;
    int events;
    size_t external_index;
} WssPoll;

typedef struct WssTransport {
    TransportKind kind;
    char *host;
    char *path;
    uint16_t port;
    uint32_t reconnect_ms;
    int tls_insecure;
    int64_t reconnect_at;
    struct lws_context *context;
    struct lws *wsi;
    int established;
    int outage_reported;
    WssMessageReceiver rx;
    WssTx *tx_head, *tx_tail;
    size_t tx_bytes;
    WssPoll *polls;
    size_t poll_count, poll_capacity;
    size_t fill_base, fill_count;
    TransportPacketCallback packet_cb;
    void *packet_opaque;
} WssTransport;

static int wss_callback(struct lws *wsi, enum lws_callback_reasons reason,
                        void *user, void *in, size_t len);
static const struct lws_protocols protocols[] = {
    { "qemu-stream", wss_callback, 0, STREAM_MAX_PACKET_SIZE + 4U, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

static char *dup_string(const char *s) {
    size_t n = strlen(s) + 1U;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}
static int poll_reserve(WssTransport *t, size_t need) {
    WssPoll *p;
    size_t cap = t->poll_capacity ? t->poll_capacity : 8U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) return -1;
        cap *= 2U;
    }
    if (cap > SIZE_MAX / sizeof(*p)) return -1;
    if (cap == t->poll_capacity) return 0;
    p = realloc(t->polls, cap * sizeof(*p));
    if (!p) return -1;
    t->polls = p;
    t->poll_capacity = cap;
    return 0;
}
static long poll_find(const WssTransport *t, lws_sockfd_type fd) {
    size_t i;
    for (i = 0; i < t->poll_count; i++)
        if (t->polls[i].fd == fd) return (long)i;
    return -1;
}
static int poll_add(WssTransport *t, const struct lws_pollargs *pa) {
    long found = poll_find(t, pa->fd);
    if (found >= 0) {
        t->polls[(size_t)found].events = pa->events;
        return 0;
    }
    if (poll_reserve(t, t->poll_count + 1U) != 0) return -1;
    t->polls[t->poll_count].fd = pa->fd;
    t->polls[t->poll_count].events = pa->events;
    t->polls[t->poll_count].external_index = SIZE_MAX;
    t->poll_count++;
    return 0;
}
static void poll_del(WssTransport *t, lws_sockfd_type fd) {
    long found = poll_find(t, fd);
    size_t i;
    if (found < 0) return;
    i = (size_t)found;
    t->poll_count--;
    if (i != t->poll_count) t->polls[i] = t->polls[t->poll_count];
}
static void poll_change(WssTransport *t, const struct lws_pollargs *pa) {
    long found = poll_find(t, pa->fd);
    if (found >= 0) t->polls[(size_t)found].events = pa->events;
}
static int external_reserve(PlatformPollFd **fds, size_t *capacity, size_t need) {
    PlatformPollFd *p;
    size_t cap = *capacity ? *capacity : 64U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) return -1;
        cap *= 2U;
    }
    if (cap > SIZE_MAX / sizeof(*p)) return -1;
    if (cap == *capacity) return 0;
    p = realloc(*fds, cap * sizeof(*p));
    if (!p) return -1;
    *fds = p;
    *capacity = cap;
    return 0;
}
static int to_platform_events(int events) {
    int out = 0;
    if (events & LWS_POLLIN) out |= PLATFORM_POLL_IN;
    if (events & LWS_POLLOUT) out |= PLATFORM_POLL_OUT;
    return out;
}
static short to_lws_revents(int events) {
    short out = 0;
    if (events & PLATFORM_POLL_IN) out |= (short)LWS_POLLIN;
    if (events & PLATFORM_POLL_OUT) out |= (short)LWS_POLLOUT;
    if (events & (PLATFORM_POLL_ERR | PLATFORM_POLL_HUP)) out |= (short)LWS_POLLHUP;
    return out;
}
static void tx_clear(WssTransport *t) {
    WssTx *q, *n;
    for (q = t->tx_head; q; q = n) { n = q->next; free(q->alloc); free(q); }
    t->tx_head = t->tx_tail = NULL; t->tx_bytes = 0;
}
static void mark_down(WssTransport *t) {
    int was_up = t->wsi != NULL || t->established;
    t->wsi = NULL; t->established = 0; tx_clear(t); wss_message_receiver_init(&t->rx);
    if (was_up) t->reconnect_at = platform_clock_ms() + t->reconnect_ms;
}
static WssTransport *from_wsi(struct lws *wsi) {
    return (WssTransport *)lws_context_user(lws_get_context(wsi));
}
static void deliver_packet(const uint8_t *p, size_t len, void *opaque) {
    WssTransport *t = opaque; t->packet_cb(p, len, t->packet_opaque);
}
static int start_connect(WssTransport *t) {
    struct lws_client_connect_info ci;
    memset(&ci, 0, sizeof(ci));
    ci.context = t->context; ci.address = t->host; ci.port = (int)t->port;
    ci.path = t->path; ci.host = t->host; ci.origin = t->host;
    /* Bind our local callback without advertising a private WebSocket
     * subprotocol to the peer.  The wire protocol is plain binary WS. */
    ci.protocol = NULL;
    ci.local_protocol_name = protocols[0].name;
    ci.ssl_connection = LCCSCF_USE_SSL;
    if (t->tls_insecure)
        ci.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    ci.pwsi = &t->wsi;
    t->wsi = lws_client_connect_via_info(&ci);
    if (!t->wsi) {
        t->reconnect_at = platform_clock_ms() + t->reconnect_ms;
        mark_down(t);
        return -1;
    }
    logger_log(LOG_LEVEL_DEBUG, "transport", "connecting", "peer=wss://%s:%u%s", t->host, (unsigned)t->port, t->path);
    return 0;
}
static int write_one(WssTransport *t, struct lws *wsi) {
    WssTx *q = t->tx_head; int n;
    if (!q) return 0;
    n = lws_write(wsi, q->alloc + LWS_PRE, q->len, LWS_WRITE_BINARY);
    if (n < (int)q->len) return -1;
    t->tx_bytes -= q->len; t->tx_head = q->next; if (!t->tx_head) t->tx_tail = NULL;
    free(q->alloc); free(q);
    if (t->tx_head) lws_callback_on_writable(wsi);
    return 0;
}
static int wss_callback(struct lws *wsi, enum lws_callback_reasons reason,
                        void *user, void *in, size_t len) {
    WssTransport *t = from_wsi(wsi); (void)user;
    if (!t) return 0;
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        t->wsi = wsi; t->established = 1; wss_message_receiver_init(&t->rx);
        if (platform_socket_set_nodelay((slirp_os_socket)lws_get_socket_fd(wsi)) != 0)
            logger_log(LOG_LEVEL_WARN, "transport", "tcp_nodelay_failed", "could not disable Nagle on WSS client socket");
        t->outage_reported = 0;
        logger_log(LOG_LEVEL_INFO, "transport", "connected", "peer=wss://%s:%u%s", t->host, (unsigned)t->port, t->path);
        if (t->tx_head) lws_callback_on_writable(wsi);
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (!lws_frame_is_binary(wsi)) return -1;
        if (wss_message_receiver_feed(&t->rx, in, len, lws_is_final_fragment(wsi), deliver_packet, t) != 0) return -1;
        break;
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (write_one(t, wsi) != 0) return -1;
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        if (!t->outage_reported) {
            logger_log(LOG_LEVEL_WARN, "transport", "connection_unavailable", "peer=wss://%s:%u%s reason=%s retry_delay_ms=%u", t->host, (unsigned)t->port, t->path, in ? (const char *)in : "connection failed", t->reconnect_ms);
            t->outage_reported = 1;
        }
        mark_down(t); break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        if (!t->outage_reported) {
            logger_log(LOG_LEVEL_WARN, "transport", "disconnected", "retry_delay_ms=%u", t->reconnect_ms);
            t->outage_reported = 1;
        }
        mark_down(t); break;
    case LWS_CALLBACK_ADD_POLL_FD:
        if (poll_add(t, (const struct lws_pollargs *)in) != 0) return -1;
        break;
    case LWS_CALLBACK_DEL_POLL_FD:
        poll_del(t, ((const struct lws_pollargs *)in)->fd);
        break;
    case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
        poll_change(t, (const struct lws_pollargs *)in);
        break;
    default: break;
    }
    return 0;
}
int wss_transport_init(void **out, const AppConfig *config, TransportPacketCallback cb, void *opaque) {
    struct lws_context_creation_info info; WssTransport *t = calloc(1, sizeof(*t));
    if (!t) return -1;
    t->kind = TRANSPORT_WSS;
    t->host = dup_string(config->stream_host); t->path = dup_string(config->ws_path);
    if (!t->host || !t->path) { wss_transport_cleanup(t); return -1; }
    t->port = config->stream_port; t->reconnect_ms = config->reconnect_ms; t->packet_cb = cb; t->packet_opaque = opaque;
    t->tls_insecure = config->tls_insecure;
    wss_message_receiver_init(&t->rx);
    memset(&info, 0, sizeof(info)); info.port = CONTEXT_PORT_NO_LISTEN; info.protocols = protocols; info.user = t;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    t->context = lws_create_context(&info); if (!t->context) { wss_transport_cleanup(t); return -1; }
    t->reconnect_at = platform_clock_ms(); *out = t; return 0;
}
void wss_transport_cleanup(void *impl) {
    WssTransport *t = impl;
    if (!t) return;
    if (t->context) lws_context_destroy(t->context);
    tx_clear(t);
    free(t->polls); free(t->host); free(t->path); free(t);
}
long wss_transport_send_ethernet(void *impl, const void *buf, size_t len) {
    WssTransport *t = impl;
    uint8_t padded[60]; const void *packet = buf; size_t packet_len = len, framed_len; WssTx *q;
    if (!t->established) return (long)len;
    if (len < sizeof(padded)) { memcpy(padded, buf, len); memset(padded + len, 0, sizeof(padded) - len); packet = padded; packet_len = sizeof(padded); }
    if (packet_len > STREAM_MAX_PACKET_SIZE || t->tx_bytes + packet_len + 4U > WSS_TX_LIMIT) return (long)len;
    q = calloc(1, sizeof(*q)); if (!q) return -1;
    q->alloc = malloc(LWS_PRE + packet_len + 4U); if (!q->alloc) { free(q); return -1; }
    if (stream_build_frame(packet, packet_len, q->alloc + LWS_PRE, packet_len + 4U, &framed_len) != 0) { free(q->alloc); free(q); return -1; }
    q->len = framed_len; if (t->tx_tail) t->tx_tail->next = q; else t->tx_head = q; t->tx_tail = q; t->tx_bytes += q->len;
    lws_callback_on_writable(t->wsi); return (long)len;
}
int wss_transport_fill_poll(void *impl, uint32_t *timeout,
                            PlatformPollFd **fds, size_t *count,
                            size_t *capacity) {
    WssTransport *t = impl;
    int64_t now = platform_clock_ms();
    size_t i;
    if (!t->wsi && now >= t->reconnect_at) (void)start_connect(t);
    t->fill_base = *count;
    t->fill_count = 0;
    if (external_reserve(fds, capacity, *count + t->poll_count) != 0) return -1;
    for (i = 0; i < t->poll_count; i++) {
        WssPoll *p = &t->polls[i];
        p->external_index = *count;
        (*fds)[*count].fd = (slirp_os_socket)p->fd;
        (*fds)[*count].events = to_platform_events(p->events);
        (*fds)[*count].revents = 0;
        (*count)++;
        t->fill_count++;
    }
    if (!t->wsi) {
        int64_t delay = t->reconnect_at - now;
        uint32_t wait = delay <= 0 ? 0U : delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
        if (wait < *timeout) *timeout = wait;
    }
    {
        int input = *timeout > INT_MAX ? INT_MAX : (int)*timeout;
        int adjusted = lws_service_adjust_timeout(t->context, input, 0);
        if (adjusted <= 0) *timeout = 0;
        else if ((uint32_t)adjusted < *timeout) *timeout = (uint32_t)adjusted;
    }
    return 0;
}

int wss_transport_poll(void *impl, PlatformPollFd *fds) {
    WssTransport *t = impl;
    WssPoll *ready;
    size_t ready_count = 0, i;
    int rc = 0;
    ready = t->fill_count ? malloc(t->fill_count * sizeof(*ready)) : NULL;
    if (t->fill_count && !ready) return -1;
    for (i = 0; i < t->fill_count; i++) {
        size_t x = t->fill_base + i;
        if (fds[x].revents) {
            ready[ready_count] = t->polls[i];
            ready[ready_count].external_index = x;
            ready_count++;
        }
    }
    for (i = 0; i < ready_count; i++) {
        struct lws_pollfd pfd;
        WssPoll *p = &ready[i];
        size_t x = p->external_index;
        if (poll_find(t, p->fd) < 0) continue;
        pfd.fd = p->fd;
        pfd.events = p->events;
        pfd.revents = to_lws_revents(fds[x].revents);
        if (lws_service_fd(t->context, &pfd) < 0) {
            rc = -1;
            break;
        }
    }
    free(ready);
    if (rc == 0 && lws_service_adjust_timeout(t->context, 1, 0) == 0 &&
        lws_service_tsi(t->context, -1, 0) < 0)
        rc = -1;
    return rc;
}

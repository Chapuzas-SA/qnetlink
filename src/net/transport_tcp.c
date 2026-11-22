#include "transport.h"
#include "protocol/stream.h"
#include "transport_wss.h"
#include "app/logger.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TxFrame { uint8_t *data; size_t len, sent; struct TxFrame *next; } TxFrame;
typedef enum ConnState { CONN_DOWN, CONN_CONNECTING, CONN_UP } ConnState;
typedef struct TcpTransport {
    TransportKind kind;
    const char *host; uint16_t port; uint32_t reconnect_ms; int64_t reconnect_at;
    slirp_os_socket sock; ConnState state; StreamReceiver rx;
    TxFrame *tx_head, *tx_tail; size_t tx_bytes;
    int outage_reported;
    TransportPacketCallback packet_cb; void *packet_opaque;
    size_t poll_index; int has_poll_fd;
} TcpTransport;

struct Transport { TransportKind kind; void *impl; };
#define TX_LIMIT (4U * 1024U * 1024U)

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
static void tx_clear(TcpTransport *t) {
    TxFrame *f, *n;
    for (f=t->tx_head; f; f=n) { n=f->next; free(f->data); free(f); }
    t->tx_head=t->tx_tail=NULL; t->tx_bytes=0;
}
static void disconnect_transport(TcpTransport *t) {
    if (t->sock != SLIRP_INVALID_SOCKET) platform_socket_close(t->sock);
    t->sock=SLIRP_INVALID_SOCKET; t->state=CONN_DOWN; tx_clear(t); stream_receiver_init(&t->rx);
    t->reconnect_at=platform_clock_ms()+t->reconnect_ms;
    if (!t->outage_reported) {
        logger_log(LOG_LEVEL_WARN, "transport", "connection_unavailable", "peer=%s:%u reconnect_delay_ms=%u", t->host, (unsigned)t->port, t->reconnect_ms);
        t->outage_reported = 1;
    }
}
static int start_connect(TcpTransport *t) {
    int connected=0;
    if (platform_tcp_connect_start(t->host,t->port,&t->sock,&connected)!=0) {
        t->reconnect_at=platform_clock_ms()+t->reconnect_ms;
        if (!t->outage_reported) {
            logger_log(LOG_LEVEL_WARN, "transport", "connection_unavailable", "peer=%s:%u reconnect_delay_ms=%u", t->host, (unsigned)t->port, t->reconnect_ms);
            t->outage_reported=1;
        }
        return -1;
    }
    t->state=connected?CONN_UP:CONN_CONNECTING;
    if (connected) { t->outage_reported=0; platform_socket_set_nodelay(t->sock); logger_log(LOG_LEVEL_INFO, "transport", "connected", "peer=%s:%u", t->host, (unsigned)t->port); }
    else logger_log(LOG_LEVEL_DEBUG, "transport", "connecting", "peer=%s:%u", t->host, (unsigned)t->port);
    return 0;
}
static void rx_packet(const uint8_t *p,size_t len,void *opaque) {
    TcpTransport *t=opaque; t->packet_cb(p,len,t->packet_opaque);
}
static int flush_tx(TcpTransport *t) {
    while (t->tx_head) {
        TxFrame *f=t->tx_head; long n=platform_socket_send(t->sock,f->data+f->sent,f->len-f->sent);
        if (n<0) { if (platform_socket_would_block()) return 0; return -1; }
        if (n==0) return -1;
        f->sent+=(size_t)n; if (f->sent<f->len) return 0;
        t->tx_bytes-=f->len; t->tx_head=f->next; if (!t->tx_head) t->tx_tail=NULL;
        free(f->data); free(f);
    }
    return 0;
}
static int read_stream(TcpTransport *t) {
    uint8_t buf[16384];
    for (;;) {
        long n=platform_socket_recv(t->sock,buf,sizeof(buf));
        if (n>0) { if (stream_receiver_feed(&t->rx,buf,(size_t)n,rx_packet,t)!=0) return -1; continue; }
        if (n==0) return -1;
        if (platform_socket_would_block()) return 0;
        return -1;
    }
}
int transport_init(Transport **out,const AppConfig *config,TransportPacketCallback cb,void *opaque) {
    Transport *t = calloc(1, sizeof(*t));
    if (!t) return -1;
    t->kind = config->transport;
    if (t->kind == TRANSPORT_WSS) {
        if (wss_transport_init(&t->impl, config, cb, opaque) != 0) { free(t); return -1; }
    } else {
        TcpTransport *tcp = calloc(1, sizeof(*tcp));
        if (!tcp) { free(t); return -1; }
        tcp->kind=TRANSPORT_TCP; tcp->host=config->stream_host; tcp->port=config->stream_port; tcp->reconnect_ms=config->reconnect_ms;
        tcp->sock=SLIRP_INVALID_SOCKET; tcp->packet_cb=cb; tcp->packet_opaque=opaque; stream_receiver_init(&tcp->rx);
        tcp->reconnect_at=platform_clock_ms(); t->impl=tcp;
    }
    *out=t; return 0;
}
void transport_cleanup(Transport *t) {
    TcpTransport *tcp;
    if (!t) return;
    if (t->kind == TRANSPORT_WSS) { wss_transport_cleanup(t->impl); free(t); return; }
    tcp=t->impl;
    if (tcp) { tx_clear(tcp); if (tcp->sock!=SLIRP_INVALID_SOCKET) platform_socket_close(tcp->sock); free(tcp); }
    free(t);
}
long transport_send_ethernet(Transport *t,const void *buf,size_t len) {
    TcpTransport *tcp; TxFrame *f; size_t framed_len; uint8_t padded[60]; const void *packet=buf; size_t packet_len=len;
    if (t->kind == TRANSPORT_WSS) return wss_transport_send_ethernet(t->impl, buf, len);
    tcp=t->impl;
    if (tcp->state!=CONN_UP) return (long)len;
    if (len<sizeof(padded)) { memcpy(padded,buf,len); memset(padded+len,0,sizeof(padded)-len); packet=padded; packet_len=sizeof(padded); }
    if (packet_len>STREAM_MAX_PACKET_SIZE || tcp->tx_bytes+packet_len+4>TX_LIMIT) return (long)len;
    f=calloc(1,sizeof(*f)); if (!f) return -1;
    f->data=malloc(packet_len+4); if (!f->data) { free(f); return -1; }
    if (stream_build_frame(packet,packet_len,f->data,packet_len+4,&framed_len)!=0) { free(f->data); free(f); return -1; }
    f->len=framed_len; if (tcp->tx_tail) tcp->tx_tail->next=f; else tcp->tx_head=f; tcp->tx_tail=f; tcp->tx_bytes+=framed_len;
    return (long)len;
}
static int ensure_capacity(PlatformPollFd **fds,size_t *capacity,size_t need) {
    PlatformPollFd *p; size_t cap=*capacity?*capacity:64;
    while (cap<need) { if (cap>SIZE_MAX/2) return -1; cap*=2; }
    if (cap==*capacity) return 0;
    p=realloc(*fds,cap*sizeof(*p)); if (!p) return -1; *fds=p; *capacity=cap; return 0;
}
int transport_fill_poll(Transport *t,uint32_t *timeout,PlatformPollFd **fds,size_t *count,size_t *capacity) {
    TcpTransport *tcp; int64_t now; uint32_t wait;
    if (t->kind == TRANSPORT_WSS)
        return wss_transport_fill_poll(t->impl, timeout, fds, count, capacity);
    tcp=t->impl; now=platform_clock_ms(); tcp->has_poll_fd=0;
    if (tcp->state==CONN_DOWN && now>=tcp->reconnect_at) start_connect(tcp);
    if (tcp->state!=CONN_DOWN) {
        if (ensure_capacity(fds,capacity,*count+1)!=0) return -1;
        tcp->poll_index=*count; tcp->has_poll_fd=1; (*fds)[*count].fd=tcp->sock; (*fds)[*count].events=PLATFORM_POLL_IN;
        if (tcp->state==CONN_CONNECTING || tcp->tx_head) (*fds)[*count].events|=PLATFORM_POLL_OUT;
        (*fds)[*count].revents=0; (*count)++;
    } else {
        int64_t d=tcp->reconnect_at-now; wait=d<=0?0:d>UINT32_MAX?UINT32_MAX:(uint32_t)d; *timeout=min_u32(*timeout,wait);
    }
    return 0;
}
int transport_poll(Transport *t,PlatformPollFd *fds) {
    TcpTransport *tcp; int re;
    if (t->kind == TRANSPORT_WSS) {
        return wss_transport_poll(t->impl, fds);
    }
    tcp=t->impl;
    if (!tcp->has_poll_fd || tcp->state==CONN_DOWN) return 0;
    re=fds[tcp->poll_index].revents;
    if (tcp->state==CONN_CONNECTING && (re&(PLATFORM_POLL_OUT|PLATFORM_POLL_ERR|PLATFORM_POLL_HUP))) {
        if (platform_tcp_connect_finish(tcp->sock)!=0) disconnect_transport(tcp);
        else { tcp->state=CONN_UP; tcp->outage_reported=0; platform_socket_set_nodelay(tcp->sock); logger_log(LOG_LEVEL_INFO, "transport", "connected", "peer=%s:%u", tcp->host, (unsigned)tcp->port); }
    } else if (tcp->state==CONN_UP) {
        if (re&(PLATFORM_POLL_ERR|PLATFORM_POLL_HUP)) disconnect_transport(tcp);
        else { if ((re&PLATFORM_POLL_IN) && read_stream(tcp)!=0) disconnect_transport(tcp); if (tcp->state==CONN_UP && (re&PLATFORM_POLL_OUT) && flush_tx(tcp)!=0) disconnect_transport(tcp); }
    }
    return 0;
}

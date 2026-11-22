#ifndef QNETLINK_TRANSPORT_WSS_H
#define QNETLINK_TRANSPORT_WSS_H
#include "transport.h"
int wss_transport_init(void **out, const AppConfig *config, TransportPacketCallback cb, void *opaque);
void wss_transport_cleanup(void *impl);
long wss_transport_send_ethernet(void *impl, const void *buf, size_t len);
int wss_transport_fill_poll(void *impl, uint32_t *timeout, PlatformPollFd **fds,
                            size_t *count, size_t *capacity);
int wss_transport_poll(void *impl, PlatformPollFd *fds);
#endif

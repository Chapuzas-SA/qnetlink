#ifndef QNETLINK_TRANSPORT_H
#define QNETLINK_TRANSPORT_H
#include "app/config.h"
#include "platform.h"
#include <stddef.h>
#include <stdint.h>
typedef void (*TransportPacketCallback)(const uint8_t *, size_t, void *);
typedef struct Transport Transport;
int transport_init(Transport **out, const AppConfig *config, TransportPacketCallback cb, void *opaque);
void transport_cleanup(Transport *t);
long transport_send_ethernet(Transport *t, const void *buf, size_t len);
int transport_fill_poll(Transport *t, uint32_t *timeout, PlatformPollFd **fds, size_t *count, size_t *capacity);
int transport_poll(Transport *t, PlatformPollFd *fds);
#endif

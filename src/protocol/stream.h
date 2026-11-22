#ifndef QNETLINK_STREAM_H
#define QNETLINK_STREAM_H
#include <stddef.h>
#include <stdint.h>
#define STREAM_HEADER_SIZE 4U
#define STREAM_MAX_PACKET_SIZE (4096U + 65536U)
typedef void (*StreamPacketCallback)(const uint8_t *, size_t, void *);
typedef struct StreamReceiver {
    uint8_t header[4]; size_t header_used;
    uint32_t packet_length; uint8_t packet[STREAM_MAX_PACKET_SIZE]; size_t packet_used;
} StreamReceiver;
void stream_receiver_init(StreamReceiver *r);
int stream_receiver_feed(StreamReceiver *r, const uint8_t *data, size_t len,
                         StreamPacketCallback cb, void *opaque);
int stream_build_frame(const uint8_t *packet, size_t len, uint8_t *out,
                       size_t capacity, size_t *out_len);
#endif

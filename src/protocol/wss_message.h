#ifndef QNETLINK_WSS_MESSAGE_H
#define QNETLINK_WSS_MESSAGE_H
#include <stddef.h>
#include <stdint.h>
#include "protocol/stream.h"
typedef void (*WssMessageCallback)(const uint8_t *ethernet, size_t len, void *opaque);
typedef struct WssMessageReceiver {
    uint8_t data[STREAM_MAX_PACKET_SIZE + 4U];
    size_t used;
} WssMessageReceiver;
void wss_message_receiver_init(WssMessageReceiver *r);
int wss_message_receiver_feed(WssMessageReceiver *r, const uint8_t *data, size_t len,
                              int final_fragment, WssMessageCallback cb, void *opaque);
#endif

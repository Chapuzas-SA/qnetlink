#include "wss_message.h"
#include <string.h>

void wss_message_receiver_init(WssMessageReceiver *r)
{
    r->used = 0;
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int wss_message_receiver_feed(WssMessageReceiver *r, const uint8_t *data, size_t len,
                              int final_fragment, WssMessageCallback cb, void *opaque)
{
    uint32_t payload_len;
    if (len > sizeof(r->data) - r->used) {
        r->used = 0;
        return -1;
    }
    if (len) {
        memcpy(r->data + r->used, data, len);
        r->used += len;
    }
    if (!final_fragment)
        return 0;
    if (r->used < 4U) {
        r->used = 0;
        return -1;
    }
    payload_len = read_be32(r->data);
    if (payload_len == 0 || payload_len > STREAM_MAX_PACKET_SIZE ||
        r->used != (size_t)payload_len + 4U) {
        r->used = 0;
        return -1;
    }
    cb(r->data + 4U, payload_len, opaque);
    r->used = 0;
    return 0;
}

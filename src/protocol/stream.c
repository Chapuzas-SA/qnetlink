#include "stream.h"
#include <string.h>
void stream_receiver_init(StreamReceiver *r) { memset(r, 0, sizeof(*r)); }
int stream_receiver_feed(StreamReceiver *r, const uint8_t *data, size_t len,
                         StreamPacketCallback cb, void *opaque)
{
    while (len) {
        if (r->header_used < 4) {
            size_t n = 4 - r->header_used; if (n > len) n = len;
            memcpy(r->header + r->header_used, data, n);
            r->header_used += n; data += n; len -= n;
            if (r->header_used < 4) continue;
            r->packet_length = ((uint32_t)r->header[0] << 24) |
                               ((uint32_t)r->header[1] << 16) |
                               ((uint32_t)r->header[2] << 8) | r->header[3];
            if (!r->packet_length || r->packet_length > STREAM_MAX_PACKET_SIZE) {
                stream_receiver_init(r);
                return -1;
            }
            r->packet_used = 0;
        }
        {
            size_t n = r->packet_length - r->packet_used; if (n > len) n = len;
            memcpy(r->packet + r->packet_used, data, n);
            r->packet_used += n; data += n; len -= n;
        }
        if (r->packet_used == r->packet_length) {
            cb(r->packet, r->packet_length, opaque);
            r->header_used = 0;
            r->packet_length = 0;
            r->packet_used = 0;
        }
    }
    return 0;
}
int stream_build_frame(const uint8_t *p, size_t len, uint8_t *out,
                       size_t cap, size_t *out_len)
{
    if (!p || !out || !out_len || !len || len > STREAM_MAX_PACKET_SIZE || cap < len + 4) return -1;
    out[0] = (uint8_t)(len >> 24); out[1] = (uint8_t)(len >> 16);
    out[2] = (uint8_t)(len >> 8); out[3] = (uint8_t)len;
    memcpy(out + 4, p, len); *out_len = len + 4; return 0;
}

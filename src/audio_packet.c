#include "audio_packet.h"

#include <string.h>

/* Byte-shifted by hand rather than htonl so this file needs no platform
 * header and builds the same on both OSes and in the unit tests. */
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

size_t swapp_audio_packet_write(uint8_t *buf, size_t size, uint32_t stream_id, uint32_t seq,
                                const uint8_t *payload, size_t payload_len) {
    if (payload_len > SWAPP_AUDIO_MAX_PAYLOAD || size < SWAPP_AUDIO_HEADER_SIZE + payload_len)
        return 0;
    put_u32(buf, SWAPP_AUDIO_MAGIC);
    put_u32(buf + 4, stream_id);
    put_u32(buf + 8, seq);
    if (payload_len > 0)
        memcpy(buf + SWAPP_AUDIO_HEADER_SIZE, payload, payload_len);
    return SWAPP_AUDIO_HEADER_SIZE + payload_len;
}

int swapp_audio_packet_parse(const uint8_t *buf, size_t len, swapp_audio_packet *out) {
    if (len < SWAPP_AUDIO_HEADER_SIZE || len - SWAPP_AUDIO_HEADER_SIZE > SWAPP_AUDIO_MAX_PAYLOAD)
        return 0;
    if (get_u32(buf) != SWAPP_AUDIO_MAGIC)
        return 0;
    out->stream_id = get_u32(buf + 4);
    out->seq = get_u32(buf + 8);
    out->payload = buf + SWAPP_AUDIO_HEADER_SIZE;
    out->payload_len = len - SWAPP_AUDIO_HEADER_SIZE;
    return 1;
}

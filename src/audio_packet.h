#ifndef SWAPP_AUDIO_PACKET_H
#define SWAPP_AUDIO_PACKET_H

#include <stddef.h>
#include <stdint.h>

/* The audio relay datagram: a 12-byte header followed by one Opus frame.
 * Every field is big-endian on the wire. See src/net.h for how the datagram
 * is carried. This file is plain C with no platform headers so the unit
 * tests can build it on their own. */
#define SWAPP_AUDIO_MAGIC 0x53574131u /* "SWA1" */
#define SWAPP_AUDIO_HEADER_SIZE 12    /* magic, stream_id, seq: u32 BE each */
#define SWAPP_AUDIO_MAX_PAYLOAD 1275  /* Opus's per-frame maximum */

typedef struct {
    uint32_t stream_id;
    uint32_t seq;
    const uint8_t *payload; /* points into the parsed buffer, not a copy */
    size_t payload_len;
} swapp_audio_packet;

/* Writes a datagram into buf. Returns the bytes written, or 0 if it does not
 * fit in size or the payload exceeds SWAPP_AUDIO_MAX_PAYLOAD. An empty
 * payload is valid: it is the probe. */
size_t swapp_audio_packet_write(uint8_t *buf, size_t size, uint32_t stream_id, uint32_t seq,
                                const uint8_t *payload, size_t payload_len);

/* Parses a received datagram. Returns 1 and fills out, or 0 to reject (too
 * short, wrong magic, oversized payload). */
int swapp_audio_packet_parse(const uint8_t *buf, size_t len, swapp_audio_packet *out);

#endif

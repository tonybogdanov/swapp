#include <stdio.h>
#include <string.h>

#include "audio_packet.h"

static int failures;

#define EXPECT(cond) \
    do { \
        if (!(cond)) { \
            printf("%s:%d: expected %s\n", __FILE__, __LINE__, #cond); \
            failures++; \
        } \
    } while (0)

static void test_packet_roundtrip(void) {
    uint8_t buf[64];
    const uint8_t payload[3] = {1, 2, 3};
    swapp_audio_packet pkt;
    EXPECT(swapp_audio_packet_write(buf, sizeof buf, 0xA1B2C3D4u, 7, payload, 3) == 15);
    EXPECT(swapp_audio_packet_parse(buf, 15, &pkt) == 1);
    EXPECT(pkt.stream_id == 0xA1B2C3D4u);
    EXPECT(pkt.seq == 7);
    EXPECT(pkt.payload_len == 3);
    EXPECT(pkt.payload_len == 3 && memcmp(pkt.payload, payload, 3) == 0);
}

static void test_packet_wire_bytes(void) {
    uint8_t buf[64];
    const uint8_t payload[3] = {1, 2, 3};
    EXPECT(swapp_audio_packet_write(buf, sizeof buf, 0xA1B2C3D4u, 7, payload, 3) == 15);
    EXPECT(buf[0] == 'S' && buf[1] == 'W' && buf[2] == 'A' && buf[3] == '1');
    EXPECT(buf[4] == 0xA1 && buf[5] == 0xB2 && buf[6] == 0xC3 && buf[7] == 0xD4);
    EXPECT(buf[8] == 0 && buf[9] == 0 && buf[10] == 0 && buf[11] == 7);
}

static void test_packet_probe(void) {
    uint8_t buf[64];
    swapp_audio_packet pkt;
    EXPECT(swapp_audio_packet_write(buf, sizeof buf, 1, 0, NULL, 0) == 12);
    EXPECT(swapp_audio_packet_parse(buf, 12, &pkt) == 1);
    EXPECT(pkt.payload_len == 0);
}

static void test_packet_rejects(void) {
    uint8_t buf[SWAPP_AUDIO_HEADER_SIZE + SWAPP_AUDIO_MAX_PAYLOAD + 1];
    swapp_audio_packet pkt;
    memset(buf, 0, sizeof buf);
    EXPECT(swapp_audio_packet_write(buf, sizeof buf, 1, 1, buf, 0) == 12);
    EXPECT(swapp_audio_packet_parse(buf, 11, &pkt) == 0);
    buf[0] = 'X';
    EXPECT(swapp_audio_packet_parse(buf, 12, &pkt) == 0);
    buf[0] = 'S';
    EXPECT(swapp_audio_packet_parse(buf, SWAPP_AUDIO_HEADER_SIZE + 1276, &pkt) == 0);
    EXPECT(swapp_audio_packet_parse(buf, SWAPP_AUDIO_HEADER_SIZE + 1275, &pkt) == 1);
}

static void test_packet_too_small(void) {
    uint8_t buf[14];
    const uint8_t payload[3] = {1, 2, 3};
    EXPECT(swapp_audio_packet_write(buf, sizeof buf, 1, 1, payload, 3) == 0);
}

int main(void) {
    test_packet_roundtrip();
    test_packet_wire_bytes();
    test_packet_probe();
    test_packet_rejects();
    test_packet_too_small();
    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}

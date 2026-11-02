#include <stdio.h>
#include <string.h>

#include "audio_jitter.h"
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

static void jpush(swapp_jitter *j, uint32_t stream, uint32_t seq) {
    uint8_t b = (uint8_t)seq;
    swapp_jitter_push(j, stream, seq, &b, 1);
}

static void jprime(swapp_jitter *j, uint32_t first) {
    for (uint32_t i = 0; i < 4; i++) jpush(j, 1, first + i);
}

/* Pops one frame and returns the result; the first payload byte lands in *b. */
static swapp_jitter_result jpop(swapp_jitter *j, int *b) {
    uint8_t out[SWAPP_AUDIO_MAX_PAYLOAD];
    size_t len = 99;
    swapp_jitter_result r = swapp_jitter_pop(j, out, &len);
    if (r == SWAPP_JITTER_PACKET) {
        EXPECT(len == 1);
        if (b) *b = out[0];
    } else {
        EXPECT(len == 0);
    }
    return r;
}

static swapp_jitter jit;

static void test_jitter_primes(void) {
    int b = -1;
    swapp_jitter_init(&jit, 4);
    for (uint32_t i = 0; i < 3; i++) jpush(&jit, 1, i);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_SILENCE);
    jpush(&jit, 1, 3);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    EXPECT(b == 0);
}

static void test_jitter_in_order(void) {
    int b = -1;
    swapp_jitter_init(&jit, 4);
    for (uint32_t i = 0; i < 10; i++) jpush(&jit, 1, i);
    for (int i = 0; i < 10; i++) {
        EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
        EXPECT(b == i);
    }
}

static void test_jitter_reorders(void) {
    int b = -1;
    const uint32_t order[4] = {0, 1, 3, 2};
    swapp_jitter_init(&jit, 4);
    for (int i = 0; i < 4; i++) jpush(&jit, 1, order[i]);
    for (int i = 0; i < 4; i++) {
        EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
        EXPECT(b == i);
    }
}

static void test_jitter_loss_is_lost(void) {
    int b = -1;
    swapp_jitter_init(&jit, 4);
    for (uint32_t i = 0; i < 4; i++) jpush(&jit, 1, i);
    jpush(&jit, 1, 5);
    for (int i = 0; i < 4; i++) EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_LOST);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    EXPECT(b == 5);
}

static void test_jitter_late_dropped(void) {
    int b = -1, before;
    swapp_jitter_init(&jit, 4);
    jprime(&jit, 0);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    before = swapp_jitter_depth(&jit);
    jpush(&jit, 1, 0);
    EXPECT(swapp_jitter_depth(&jit) == before);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    EXPECT(b == 2);
}

static void test_jitter_duplicate_ignored(void) {
    swapp_jitter_init(&jit, 4);
    jpush(&jit, 1, 0);
    jpush(&jit, 1, 0);
    for (uint32_t i = 1; i < 4; i++) jpush(&jit, 1, i);
    EXPECT(swapp_jitter_depth(&jit) == 4);
}

static void test_jitter_new_stream_resets(void) {
    int b = -1;
    swapp_jitter_init(&jit, 4);
    jprime(&jit, 0);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    for (uint32_t i = 500; i < 504; i++) jpush(&jit, 2, i);
    EXPECT(swapp_jitter_stream(&jit) == 2);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    EXPECT(b == (uint8_t)500);
}

static void test_jitter_far_jump_resyncs(void) {
    int b = -1;
    swapp_jitter_init(&jit, 4);
    jprime(&jit, 10);
    jpush(&jit, 1, 10 + SWAPP_JITTER_SLOTS);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_SILENCE);
    for (uint32_t i = 75; i < 78; i++) jpush(&jit, 1, i);
    EXPECT(swapp_jitter_depth(&jit) == 4);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    EXPECT(b == 74);
}

static void test_jitter_reprimes_after_gap(void) {
    int b = -1;
    swapp_jitter_init(&jit, 4);
    jprime(&jit, 0);
    for (int i = 0; i < 4; i++) EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    for (int i = 0; i < SWAPP_JITTER_MAX_LOST - 1; i++) EXPECT(jpop(&jit, &b) == SWAPP_JITTER_LOST);
    EXPECT(jpop(&jit, &b) == SWAPP_JITTER_SILENCE);
}

static void test_jitter_burst_after_gap(void) {
    int b = -1;
    swapp_jitter_init(&jit, 4);
    jprime(&jit, 0);
    for (int i = 0; i < 4; i++) EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
    for (int i = 0; i < 4; i++) EXPECT(jpop(&jit, &b) == SWAPP_JITTER_LOST);
    for (uint32_t i = 8; i < 13; i++) jpush(&jit, 1, i);
    for (int i = 8; i < 13; i++) {
        EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
        EXPECT(b == i);
    }
}

static void test_jitter_probe_ignored(void) {
    uint8_t b = 0;
    swapp_jitter_init(&jit, 4);
    swapp_jitter_push(&jit, 7, 0, &b, 0);
    EXPECT(swapp_jitter_depth(&jit) == 0);
    EXPECT(swapp_jitter_stream(&jit) == 0);
    jpush(&jit, 1, 0);
    swapp_jitter_push(&jit, 1, 1, &b, SWAPP_AUDIO_MAX_PAYLOAD + 1);
    EXPECT(swapp_jitter_depth(&jit) == 1);
}

static void test_jitter_seq_wraps(void) {
    int b = -1;
    swapp_jitter_init(&jit, 4);
    jprime(&jit, 0xFFFFFFFEu);
    for (int i = 0; i < 4; i++) {
        EXPECT(jpop(&jit, &b) == SWAPP_JITTER_PACKET);
        EXPECT(b == (uint8_t)(0xFFFFFFFEu + (uint32_t)i));
    }
}

static double run_drift(swapp_drift *d, int n, double fill, double target) {
    double r = 1.0;
    for (int i = 0; i < n; i++) r = swapp_drift_update(d, fill, target);
    return r;
}

static void test_drift_at_target(void) {
    swapp_drift d;
    swapp_drift_reset(&d);
    EXPECT(run_drift(&d, 500, 40, 40) == 1.0);
}

static void test_drift_too_full_speeds(void) {
    swapp_drift d;
    double r;
    swapp_drift_reset(&d);
    r = run_drift(&d, 500, 60, 40);
    EXPECT(r > 1.0019 && r < 1.0021);
}

static void test_drift_too_empty_slows(void) {
    swapp_drift d;
    double r;
    swapp_drift_reset(&d);
    r = run_drift(&d, 500, 20, 40);
    EXPECT(r < 0.9981 && r > 0.9979);
}

static void test_drift_clamped(void) {
    swapp_drift d;
    swapp_drift_reset(&d);
    EXPECT(run_drift(&d, 500, 1000, 40) == 1.005);
}

static void test_drift_step(void) {
    swapp_drift d;
    double r;
    swapp_drift_reset(&d);
    run_drift(&d, 200, 40, 40);
    r = run_drift(&d, 500, 60, 40);
    EXPECT(r > 1.0019 && r < 1.0020);
}

static void test_drift_spike_smoothed(void) {
    swapp_drift d;
    swapp_drift_reset(&d);
    run_drift(&d, 200, 40, 40);
    EXPECT(swapp_drift_update(&d, 140, 40) < 1.0002);
}

int main(void) {
    test_jitter_primes();
    test_jitter_in_order();
    test_jitter_reorders();
    test_jitter_loss_is_lost();
    test_jitter_late_dropped();
    test_jitter_duplicate_ignored();
    test_jitter_new_stream_resets();
    test_jitter_far_jump_resyncs();
    test_jitter_reprimes_after_gap();
    test_jitter_burst_after_gap();
    test_jitter_probe_ignored();
    test_jitter_seq_wraps();
    test_drift_at_target();
    test_drift_too_full_speeds();
    test_drift_too_empty_slows();
    test_drift_clamped();
    test_drift_spike_smoothed();
    test_drift_step();
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

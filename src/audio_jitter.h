#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_packet.h"

/* 640 ms of 10 ms frames: anything further ahead than this is a jump, not reordering. */
#define SWAPP_JITTER_SLOTS 64
/* Consecutive losses (200 ms) after which the sender is presumed gone and we re-prime. */
#define SWAPP_JITTER_MAX_LOST 20

typedef enum { SWAPP_JITTER_SILENCE, SWAPP_JITTER_PACKET, SWAPP_JITTER_LOST } swapp_jitter_result;

typedef struct {
    uint32_t seq[SWAPP_JITTER_SLOTS];
    uint16_t len[SWAPP_JITTER_SLOTS];
    uint8_t used[SWAPP_JITTER_SLOTS];
    uint8_t payload[SWAPP_JITTER_SLOTS][SWAPP_AUDIO_MAX_PAYLOAD];
    uint32_t stream_id;
    int has_stream;
    uint32_t next_seq;
    int priming;
    int lost_run;
    int target_frames;
} swapp_jitter;

void swapp_jitter_init(swapp_jitter *j, int target_frames);
void swapp_jitter_push(swapp_jitter *j, uint32_t stream_id, uint32_t seq, const uint8_t *data, size_t len);
/* `out` must hold SWAPP_AUDIO_MAX_PAYLOAD bytes for a PACKET; *out_len is 0 for SILENCE and LOST. */
swapp_jitter_result swapp_jitter_pop(swapp_jitter *j, uint8_t *out, size_t *out_len);
/* Frames from next_seq up to and including the newest held packet, 0 when none is held. */
int swapp_jitter_depth(const swapp_jitter *j);
/* Changes when a new sender appears, so the caller knows to reset its decoder. */
uint32_t swapp_jitter_stream(const swapp_jitter *j);

/* Holds the delay the relay buffers steady by nudging the playback rate: the
 * two machines' clocks differ by a few parts per million, so without it the
 * buffer slowly drains or grows over hours. */
typedef struct {
    double smoothed_ms;
    int primed;
} swapp_drift;

void swapp_drift_reset(swapp_drift *d);
/* Playback-rate ratio, called once per 10 ms frame with the buffered delay in ms. */
double swapp_drift_update(swapp_drift *d, double fill_ms, double target_ms);

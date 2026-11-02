#pragma once

#include <stddef.h>
#include <stdint.h>

#include "audio_packet.h"

/* 640 ms of 10 ms frames: anything further ahead than this is a jump, not reordering. */
#define SWAPP_JITTER_SLOTS 64
/* Consecutive concealed frames (200 ms), stalled or lost, after which the
 * sender is presumed gone and we re-prime. */
#define SWAPP_JITTER_MAX_LOST 20

/* LOST means "conceal this frame": the packet is either lost or late. */
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
    /* Cumulative since init: frames concealed because their packet had not
     * arrived yet, plus packets that arrived after their slot was played;
     * and frames skipped because a later packet showed theirs was lost. */
    uint32_t late;
    uint32_t lost;
} swapp_jitter;

void swapp_jitter_init(swapp_jitter *j, int target_frames);
/* The depth priming waits for, and the re-prime backlog trim keeps. */
void swapp_jitter_set_target(swapp_jitter *j, int target_frames);
void swapp_jitter_push(swapp_jitter *j, uint32_t stream_id, uint32_t seq, const uint8_t *data, size_t len);
/* `out` must hold SWAPP_AUDIO_MAX_PAYLOAD bytes for a PACKET; *out_len is 0 for SILENCE and LOST. */
swapp_jitter_result swapp_jitter_pop(swapp_jitter *j, uint8_t *out, size_t *out_len);
/* Frames from next_seq up to and including the newest held packet, 0 when none is held. */
int swapp_jitter_depth(const swapp_jitter *j);
/* Changes when a new sender appears, so the caller knows to reset its decoder. */
uint32_t swapp_jitter_stream(const swapp_jitter *j);

/* Measures how late datagrams arrive, to size the jitter buffer: the lowest
 * delay that would have played the last minute without a gap. Each arrival's
 * lateness is D = arrival_ms - seq * 10, the arrival time against the
 * stream's own clock; only differences in D matter, so the offset between the
 * two machines' clocks drops out. Kept as min and max D per wall-clock
 * second for the last SWAPP_ADAPT_SECONDS seconds, so a spike raises the
 * target at once and lowers it only when it ages out. */
#define SWAPP_ADAPT_SECONDS 60
#define SWAPP_ADAPT_MARGIN_MS 10
/* An arrival this long after the previous one starts a new window: a capture
 * pause (music stopped, the sink suspended) shifts D for good without seq
 * advancing, and would otherwise pin the target high for the whole window. */
#define SWAPP_ADAPT_GAP_MS 500

typedef struct {
    int has_stream;
    uint32_t stream_id;
    uint32_t base_seq; /* D is measured from the stream's first seq, so it never wraps */
    int64_t first_ms;
    int64_t last_ms;
    int64_t second[SWAPP_ADAPT_SECONDS]; /* the second each bucket holds, -1 for none */
    int64_t min_d[SWAPP_ADAPT_SECONDS];
    int64_t max_d[SWAPP_ADAPT_SECONDS];
} swapp_adapt;

void swapp_adapt_reset(swapp_adapt *a);
/* One accepted datagram; now_ms is a monotonic clock. A new stream_id, or a
 * gap longer than SWAPP_ADAPT_GAP_MS since the previous arrival, resets. */
void swapp_adapt_arrival(swapp_adapt *a, uint32_t stream_id, uint32_t seq, int64_t now_ms);
/* The jitter target in ms: a multiple of 10 within
 * [SWAPP_AUDIO_TARGET_MIN_MS, SWAPP_AUDIO_TARGET_MAX_MS], or
 * SWAPP_AUDIO_TARGET_START_MS until a second of data exists. */
int swapp_adapt_target_ms(const swapp_adapt *a, int64_t now_ms);

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

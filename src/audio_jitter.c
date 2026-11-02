#include "audio_jitter.h"

#include <string.h>

/* Every comparison goes through a signed difference so sequence wrap-around is harmless. */
static int32_t seq_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void reset_to(swapp_jitter *j, uint32_t seq) {
    memset(j->used, 0, sizeof j->used);
    j->next_seq = seq;
    j->priming = 1;
    j->lost_run = 0;
}

void swapp_jitter_init(swapp_jitter *j, int target_frames) {
    memset(j->used, 0, sizeof j->used);
    j->stream_id = 0;
    j->has_stream = 0;
    j->next_seq = 0;
    j->priming = 1;
    j->lost_run = 0;
    j->target_frames = target_frames;
}

int swapp_jitter_depth(const swapp_jitter *j) {
    int depth = 0;
    for (int i = 0; i < SWAPP_JITTER_SLOTS; i++) {
        int32_t d;
        if (!j->used[i]) continue;
        d = seq_diff(j->seq[i], j->next_seq);
        if (d >= 0 && d < SWAPP_JITTER_SLOTS && d + 1 > depth) depth = d + 1;
    }
    return depth;
}

uint32_t swapp_jitter_stream(const swapp_jitter *j) { return j->stream_id; }

void swapp_jitter_push(swapp_jitter *j, uint32_t stream_id, uint32_t seq, const uint8_t *data, size_t len) {
    size_t slot;
    /* An empty payload is the client's reachability probe: it carries no audio. */
    if (len == 0 || len > SWAPP_AUDIO_MAX_PAYLOAD) return;

    if (!j->has_stream || stream_id != j->stream_id) {
        j->stream_id = stream_id;
        j->has_stream = 1;
        reset_to(j, seq);
    } else if (seq_diff(seq, j->next_seq) < 0) {
        return; /* too late: its slot has already been played or declared lost */
    } else if (seq_diff(seq, j->next_seq) >= SWAPP_JITTER_SLOTS) {
        reset_to(j, seq);
    }

    slot = seq % SWAPP_JITTER_SLOTS;
    if (j->used[slot] && j->seq[slot] == seq) return; /* duplicate */
    memcpy(j->payload[slot], data, len);
    j->len[slot] = (uint16_t)len;
    j->seq[slot] = seq;
    j->used[slot] = 1;
}

swapp_jitter_result swapp_jitter_pop(swapp_jitter *j, uint8_t *out, size_t *out_len) {
    size_t slot;
    *out_len = 0;

    if (j->priming) {
        if (swapp_jitter_depth(j) < j->target_frames) return SWAPP_JITTER_SILENCE;
        j->priming = 0;
        j->lost_run = 0;
    }

    slot = j->next_seq % SWAPP_JITTER_SLOTS;
    if (j->used[slot] && j->seq[slot] == j->next_seq) {
        memcpy(out, j->payload[slot], j->len[slot]);
        *out_len = j->len[slot];
        j->used[slot] = 0;
        j->next_seq++;
        j->lost_run = 0;
        return SWAPP_JITTER_PACKET;
    }

    j->next_seq++;
    if (++j->lost_run >= SWAPP_JITTER_MAX_LOST) {
        /* Nothing for 200 ms: stop concealing and wait for the stream to refill. */
        j->priming = 1;
        j->lost_run = 0;
        return SWAPP_JITTER_SILENCE;
    }
    return SWAPP_JITTER_LOST;
}

void swapp_drift_reset(swapp_drift *d) {
    d->smoothed_ms = 0;
    d->primed = 0;
}

double swapp_drift_update(swapp_drift *d, double fill_ms, double target_ms) {
    double adjust;
    if (!d->primed) {
        d->smoothed_ms = fill_ms;
        d->primed = 1;
    } else {
        /* About a 1 s time constant at 100 calls a second, so a burst or a
         * stall in the network does not turn into an audible pitch bend. */
        d->smoothed_ms += 0.01 * (fill_ms - d->smoothed_ms);
    }
    /* 10 ms of error is 0.1% (about 1.7 cents); never more than 0.5%. */
    adjust = (d->smoothed_ms - target_ms) * 0.0001;
    if (adjust > 0.005) adjust = 0.005;
    if (adjust < -0.005) adjust = -0.005;
    return 1.0 + adjust;
}

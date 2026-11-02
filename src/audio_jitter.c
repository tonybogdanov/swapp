#include "audio_jitter.h"

#include <string.h>

#include "audio.h"

/* Every comparison goes through a signed difference so sequence wrap-around is harmless. */
static int32_t seq_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void reset_to(swapp_jitter *j, uint32_t seq) {
    memset(j->used, 0, sizeof j->used);
    j->next_seq = seq;
    j->priming = 1;
    j->lost_run = 0;
    j->stall_run = 0;
}

void swapp_jitter_init(swapp_jitter *j, int target_frames) {
    memset(j->used, 0, sizeof j->used);
    j->stream_id = 0;
    j->has_stream = 0;
    j->next_seq = 0;
    j->priming = 1;
    j->lost_run = 0;
    j->stall_run = 0;
    j->target_frames = target_frames;
    j->late = 0;
    j->lost = 0;
}

void swapp_jitter_set_target(swapp_jitter *j, int target_frames) { j->target_frames = target_frames; }

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
    /* An empty payload carries no audio, so it is never queued. */
    if (len == 0 || len > SWAPP_AUDIO_MAX_PAYLOAD) return;

    if (!j->has_stream || stream_id != j->stream_id) {
        j->stream_id = stream_id;
        j->has_stream = 1;
        reset_to(j, seq);
    } else if (seq_diff(seq, j->next_seq) < 0) {
        /* Too late: its slot has already been played or declared lost. */
        j->late++;
        return;
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
        int depth = swapp_jitter_depth(j);
        if (depth < j->target_frames) return SWAPP_JITTER_SILENCE;
        /* A WiFi stall long enough to re-prime ends with its whole backlog
         * arriving at once. Playing all of it would add that much delay, which
         * drift control can only shed at 0.5% (80 s for 400 ms), so a backlog
         * past twice the target starts at the target depth and drops the older
         * frames. A smaller excess is kept: drift control sheds it in seconds,
         * and dropping audio is audible. */
        if (depth > 2 * j->target_frames) j->next_seq += (uint32_t)(depth - j->target_frames);
        j->priming = 0;
        j->lost_run = 0;
    }

    slot = j->next_seq % SWAPP_JITTER_SLOTS;
    if (j->stall_run > 0 && !(j->used[slot] && j->seq[slot] == j->next_seq) &&
        swapp_jitter_depth(j) > 0) {
        /* A stall on this slot ends with a later packet: this one was lost,
         * not late. The stall already concealed a frame for it, so move past
         * it without concealing again and play the next slot now; the rest
         * of the stall delayed everything after it, so those frames were
         * late. */
        j->late += j->stall_run - 1;
        j->lost++;
        j->stall_run = 0;
        j->next_seq++;
        slot = j->next_seq % SWAPP_JITTER_SLOTS;
    }
    if (j->used[slot] && j->seq[slot] == j->next_seq) {
        memcpy(out, j->payload[slot], j->len[slot]);
        *out_len = j->len[slot];
        j->used[slot] = 0;
        j->next_seq++;
        j->lost_run = 0;
        j->late += j->stall_run; /* the stall resolved: its packet came late */
        j->stall_run = 0;
        return SWAPP_JITTER_PACKET;
    }

    /* An empty slot is either a loss or a stall. A later packet already held
     * means this one went missing: conceal it and move on. Nothing held means
     * the network is stalled and this packet is still coming: conceal the
     * frame but keep the slot, so the packet plays when it arrives and the
     * stall grows the buffer instead of dropping audio. */
    if (swapp_jitter_depth(j) > 0) {
        j->next_seq++;
        j->lost++;
    } else {
        j->stall_run++;
    }
    if (++j->lost_run >= SWAPP_JITTER_MAX_LOST) {
        /* Nothing for 200 ms: stop concealing and wait for the stream to
         * refill. A stall this long is the sender pausing (the capture
         * suspended), not packets running late, so it counts as neither. */
        j->priming = 1;
        j->lost_run = 0;
        j->stall_run = 0;
        return SWAPP_JITTER_SILENCE;
    }
    return SWAPP_JITTER_LOST;
}

void swapp_adapt_reset(swapp_adapt *a) {
    a->has_stream = 0;
    a->stream_id = 0;
    a->base_seq = 0;
    a->first_ms = 0;
    a->last_ms = 0;
    for (int i = 0; i < SWAPP_ADAPT_SECONDS; i++) a->second[i] = -1;
}

int swapp_adapt_arrival(swapp_adapt *a, uint32_t stream_id, uint32_t seq, int64_t now_ms) {
    int64_t d, second;
    int i, restarted = 0;
    if (!a->has_stream || stream_id != a->stream_id || now_ms - a->last_ms > SWAPP_ADAPT_GAP_MS) {
        /* A new sender's timing says nothing about the old one's, and after a
         * pause neither does the same sender's: D has shifted by however long
         * the capture stopped. A WiFi stall that long has already re-primed
         * the jitter buffer, so its lost sample costs little. */
        swapp_adapt_reset(a);
        a->has_stream = 1;
        a->stream_id = stream_id;
        a->base_seq = seq;
        a->first_ms = now_ms;
        restarted = 1;
    }
    a->last_ms = now_ms;
    d = now_ms - (int64_t)seq_diff(seq, a->base_seq) * 10;
    second = now_ms / 1000;
    i = (int)(second % SWAPP_ADAPT_SECONDS);
    if (a->second[i] != second) {
        a->second[i] = second;
        a->min_d[i] = d;
        a->max_d[i] = d;
    } else {
        if (d < a->min_d[i]) a->min_d[i] = d;
        if (d > a->max_d[i]) a->max_d[i] = d;
    }
    return restarted;
}

int swapp_adapt_target_ms(const swapp_adapt *a, int64_t now_ms) {
    int64_t second = now_ms / 1000, lo = 0, hi = 0, target;
    int any = 0;
    if (!a->has_stream || now_ms - a->first_ms < 1000) return SWAPP_AUDIO_TARGET_START_MS;
    for (int i = 0; i < SWAPP_ADAPT_SECONDS; i++) {
        if (a->second[i] < 0 || a->second[i] > second || second - a->second[i] >= SWAPP_ADAPT_SECONDS)
            continue;
        if (!any || a->min_d[i] < lo) lo = a->min_d[i];
        if (!any || a->max_d[i] > hi) hi = a->max_d[i];
        any = 1;
    }
    if (!any) return SWAPP_AUDIO_TARGET_START_MS;
    /* The spread is how much later than its earliest the latest packet came:
     * a buffer that deep would have played every one of them. */
    target = hi - lo + SWAPP_ADAPT_MARGIN_MS;
    target = (target + 9) / 10 * 10;
    if (target < SWAPP_AUDIO_TARGET_MIN_MS) target = SWAPP_AUDIO_TARGET_MIN_MS;
    if (target > SWAPP_AUDIO_TARGET_MAX_MS) target = SWAPP_AUDIO_TARGET_MAX_MS;
    return (int)target;
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

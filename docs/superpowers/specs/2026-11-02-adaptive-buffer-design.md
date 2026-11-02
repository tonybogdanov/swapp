# Adaptive relay buffer

Follows the audio relay (`2026-11-02-audio-relay-design.md`). Server (Windows) only; the client is untouched.

## Why

A fixed 40 ms jitter target glitched under WiFi stalls and CPU load. A fixed 120 ms target is clean but wastes delay when the link is calm. The user wants the lowest delay that stays glitch-free, with a total delay of up to 200 ms acceptable. They also want less text in the UI.

## Behaviour

**Two kinds of gap.** Every datagram carries a sequence number, so the server can tell them apart when it is about to play slot `next_seq` and finds it empty:
- **Loss:** a later packet is already held. The missing one is presumed lost. Conceal it with PLC, advance and count it as *lost*. This is today's behaviour.
- **Stall:** the buffer holds nothing ahead. Conceal with PLC but **do not advance**. When the stalled packets arrive they still play, so the stall grows the buffer instead of dropping audio. Count each such frame as *late*.

`SWAPP_JITTER_MAX_LOST` (20 consecutive concealed frames, whether stall or loss, leading to a re-prime) stays as it is. A packet arriving behind `next_seq` is dropped as today and also counts as *late*.

**Target from measured lateness.** For each accepted datagram the receive thread records `D = arrival_ms - seq * 10`, the arrival time relative to the stream's own clock, with `arrival_ms` from QueryPerformanceCounter.
- Keep one-second buckets of min D and max D for the last 60 s.
- `target_ms = max(maxD) - min(minD) + 10 ms margin` over the window, rounded up to a 10 ms multiple and clamped to [20, 150] ms. The device buffer of about 47 ms comes on top, so the total stays at or under about 200 ms.
- Before 1 s of data exists, or after a stream change, which resets the window, the target is 100 ms.
- Growth is immediate, because a new max lands in the current bucket. Shrinking happens only when a spike ages out of the 60 s window. The existing drift controller then sheds the excess at up to 0.5%, so it never jumps or drops audio.

The jitter buffer's `target_frames` and the drift controller's target both follow this value: drift target = adaptive target + actual device buffer. The re-prime backlog trim (keeping more than 2× target) uses the current target. `SWAPP_AUDIO_TARGET_MS` goes away, replaced by MIN/MAX/START constants.

## UI (Windows audio row only)

- **Text:** `Audio: <device> · 74 ms`, where the number is the smoothed buffered delay. Other states keep their current text.
- **Icon:** the existing check when no late or lost frames occurred in the last 60 s. The check is tinted amber when any did, the same way `SWAPP_ICON_CHECK_MUTED` tints gray, so no new asset is needed. Spinner and error stay as today.
- **Hover tooltip on the row:** three lines.
  - `Buffer 74 ms, target 70 ms`
  - `Last minute: 0 late, 2 lost`
  - `Drift control on`, or `off`

  The ` (no drift control)` suffix moves from the text into this tooltip.

## Out of scope

Client-side changes, FEC, raw PCM, and a user-set delay.

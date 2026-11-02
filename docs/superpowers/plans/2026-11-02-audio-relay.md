# Audio Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The laptop's `Swapp Audio Relay` output device streams Opus over UDP to the Swapp server, which plays it on the PC's default output.

**Architecture:** A new `audio` module, split the same way as `net`: a shared contract (`src/audio.h`), the client in `src/audio_linux.c` (libpulse null sink, monitor capture, Opus encode, UDP send) and the server in `src/audio_win.c` (UDP receive, jitter buffer, Opus decode, WASAPI render). The wire format, jitter buffer and drift controller are pure C in `src/audio_packet.c` and `src/audio_jitter.c`, so they can be unit-tested without audio hardware. The TCP link stays as it is. It only gains an accessor that tells the audio module who the peer is.

**Tech Stack:** C11, CMake, Opus 1.5.2 (FetchContent, static), libpulse (client), WASAPI/MMDevice through C COM (server), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-11-02-audio-relay-design.md`

## Global Constraints

- Opus: 48 kHz, stereo, 256 kbps, `OPUS_APPLICATION_AUDIO`, 10 ms frames (480 samples per channel).
- UDP port 21423 (`SWAPP_NET_PORT`). Only the client sends; the server never sends UDP.
- The server accepts a datagram only if its source IP is the currently connected TCP client's. Everything else is dropped before parsing.
- Client UDP socket is bound to the local address of the live TCP socket (the LAN source address), never left unbound.
- Null sink: `sink_name=swapp_relay`, description `Swapp Audio Relay`. Verified quoting on PipeWire 1.4.2 (`pactl info`): `sink_properties='device.description="Swapp Audio Relay"'`. The unquoted form gives just "Swapp".
- Verified on this laptop (corrected during Task 5): the null sink's monitor **already applies the sink volume** (pipewire-pulse sets `monitor.channel-volumes` on it: a 0.5 tone recorded 0.5 at 100%, 0.0135 at 30%, 0 muted). The client must NOT apply any gain of its own. An earlier measurement said the opposite and was wrong.
- The relay device is reused if it already exists, and removed only on the tray's Quit menu item. Update handover (`swapp_tray_stop_running` sending `'q'`, which calls `swapp_tray_on_quit(NULL, NULL)`) keeps it loaded.
- `swapp_audio_stop()` returns within 500 ms no matter what. Nothing in audio may delay quit or Windows shutdown.
- Every push to `main` replaces the only release. Each stage's last task leaves both binaries working as they do today plus the new piece.
- There is no local full build (the dev libraries are missing on the laptop). Unit tests build locally with plain `gcc`. Windows sources are syntax-checked locally with `x86_64-w64-mingw32-gcc -fsyntax-only`. CI is the real build.
- Windows-side checks go through `/mnt/pi`: write a `.ps1` there, the user runs it on the PC, and it writes its output next to itself (`$PSScriptRoot`).

## Review Focus

1. **Laptop sleep/resume or WiFi change while streaming.** Expected: the stream resumes by itself once the TCP link reconnects, and the server re-primes cleanly instead of playing a burst of stale audio. Pinned by `test_jitter_new_stream_resets` and `test_jitter_far_jump_resyncs` (Task 4).
2. **PipeWire restarting under the client (`systemctl --user restart pipewire pipewire-pulse`).** Expected: the device comes back and capture resumes within a few seconds, without a duplicate device. Hardware check in Task 5.
3. **RCF powered off and on, or default device changed, mid-song.** Expected: playback moves to the new default within about a second. Hardware check in Task 6.
4. **Bursty WiFi delivering 5 packets at once after a 50 ms gap.** Expected: no reset, the buffer absorbs it, and the late-but-in-window packets play. Pinned by `test_jitter_burst_after_gap` (Task 4).
5. **Hours of music.** Expected: the buffered delay shown in the server status stays near target. Pinned by the `test_drift_*` tests (Task 7), with an hour-long hardware check.

---

## Stage 1: UDP probe (one push, after Task 3)

### Task 1: Wire format and the unit-test target

**Files:**
- Create: `src/audio_packet.h`, `src/audio_packet.c`, `tests/audio_tests.c`
- Modify: `CMakeLists.txt`, `.github/workflows/release.yml` (Linux job), `src/net.h` (protocol doc)

**Interfaces:**
- Produces (`src/audio_packet.h`):
  ```c
  #define SWAPP_AUDIO_MAGIC 0x53574131u        /* "SWA1", big-endian on the wire */
  #define SWAPP_AUDIO_HEADER_SIZE 12            /* magic, stream_id, seq: u32 BE each */
  #define SWAPP_AUDIO_MAX_PAYLOAD 1275          /* Opus's per-frame maximum */
  typedef struct { uint32_t stream_id; uint32_t seq; const uint8_t *payload; size_t payload_len; } swapp_audio_packet;
  size_t swapp_audio_packet_write(uint8_t *buf, size_t size, uint32_t stream_id, uint32_t seq,
                                  const uint8_t *payload, size_t payload_len); /* bytes written, 0 if it won't fit or payload too big */
  int swapp_audio_packet_parse(const uint8_t *buf, size_t len, swapp_audio_packet *out); /* 1 ok, 0 reject */
  ```
  An empty payload is valid: that is the stage-1 probe.

- [ ] **Step 1: Write the failing tests** in `tests/audio_tests.c`. It is a plain `main()` that runs each `test_*` function, uses an `EXPECT(cond)` macro printing file:line on failure, and returns nonzero if any test failed.
  ```c
  test_packet_roundtrip:   write(stream 0xA1B2C3D4, seq 7, 3-byte payload {1,2,3}) == 15; parse ok; fields equal; payload bytes equal
  test_packet_wire_bytes:  buf[0..3] == 'S','W','A','1'; buf[4..7] == A1 B2 C3 D4; buf[8..11] == 00 00 00 07
  test_packet_probe:       write with payload_len 0 == 12; parse ok; payload_len == 0
  test_packet_rejects:     parse of 11 bytes == 0; bad magic == 0; header + 1276-byte payload == 0
  test_packet_too_small:   write into a 14-byte buffer with a 3-byte payload == 0
  ```
- [ ] **Step 2: Run, expect a compile failure.** `gcc -std=c11 -Wall -Wextra -Isrc tests/audio_tests.c src/audio_packet.c -o /tmp/claude-1000/audio_tests` fails with an undefined `swapp_audio_packet_write`.
- [ ] **Step 3: Implement `src/audio_packet.c`.** Byte-shift the big-endian encoding by hand. No `htonl`, so the file has no platform headers.
- [ ] **Step 4: Run, expect pass.** Same command, then `/tmp/claude-1000/audio_tests` prints `all passed` and exits 0.
- [ ] **Step 5: CMake and CI.** Add `src/audio_packet.c` to the `swapp` sources. Add `enable_testing()`, `add_executable(swapp_audio_tests tests/audio_tests.c src/audio_packet.c)` with `src` as an include directory, and `add_test(NAME audio COMMAND swapp_audio_tests)`. In the Linux CI job, after Build, add a step `ctest --test-dir build-linux --output-on-failure`.
- [ ] **Step 6: Document the datagram in `src/net.h`.** Add a block comment after the transport notes covering: UDP on the same port number; client to server only; the 12-byte header; one Opus frame (10 ms) per datagram; an empty payload is a probe; the server's source-IP rule; and why the audio isn't on the TCP link (WiFi retransmit stalls, `nc`).
- [ ] **Step 7: Commit.** `feat: add the audio relay datagram format`

### Task 2: Peer accessor on the link

**Files:**
- Modify: `src/net.h`, `src/net_linux.c`, `src/net_win.c`

**Interfaces:**
- Produces (`src/net.h`):
  ```c
  /* Nonzero while connected. peer_be: the other side's IPv4 address; self_be: this
   * side's address on the same connection (getsockname of the TCP socket). Both in
   * network byte order. Safe from any thread. */
  int swapp_net_peer(uint32_t *peer_be, uint32_t *self_be);
  ```

- [ ] **Step 1: Implement on Linux.** In `swapp_net_serve`, record the peer address and the TCP socket's `getsockname` under `g_state_lock` when the connection is set up, and clear them where `g_sock = -1`. `swapp_net_peer` reads them under the lock.
- [ ] **Step 2: Implement on Windows.** Do the same in `net_win.c`'s `swapp_net_serve` under `g_state_lock` (the peer comes from `accept`'s `from`).
- [ ] **Step 3: Syntax check.** `x86_64-w64-mingw32-gcc -std=c11 -fsyntax-only -Isrc src/net_win.c` reports no errors in the new code. Pre-existing `sscanf_s` warnings are acceptable.
- [ ] **Step 4: Commit.** `feat: expose the link's peer and local address`

### Task 3: Audio module skeleton, UDP probe and status rows

**Files:**
- Create: `src/audio.h`, `src/audio_linux.c`, `src/audio_win.c`
- Modify: `src/tray_linux.c` (rows ~1374, refresh ~1213, quit ~1512, startup ~1913), `src/tray_win.c` (rows ~1567 and frame height ~1596, refresh ~932, messages ~36 and ~2037, startup ~2189, teardown ~2212), `CMakeLists.txt`

**Interfaces:**
- Consumes: `swapp_net_peer` (Task 2), `swapp_audio_packet_*` (Task 1).
- Produces (`src/audio.h`):
  ```c
  #define SWAPP_AUDIO_RATE 48000
  #define SWAPP_AUDIO_CHANNELS 2
  #define SWAPP_AUDIO_BITRATE 256000
  #define SWAPP_AUDIO_FRAME 480            /* samples per channel per datagram: 10 ms */
  #define SWAPP_AUDIO_TARGET_MS 40         /* jitter-buffer target; tuned in Stage 2 */
  #define SWAPP_AUDIO_SILENT_MS 3000       /* server: connected client, no datagram for this long */
  void swapp_audio_start(void);
  void swapp_audio_stop(int remove_device); /* bounded to 500 ms; remove_device matters only on the client */
  int  swapp_audio_ok(void);                /* drives the row icon: check vs spinner */
  void swapp_audio_status_text(char *buf, size_t buf_size);
  void swapp_audio_set_state_callback(void (*callback)(void));
  ```
  The callback contract mirrors `swapp_net_set_state_callback`. On Linux it runs on the GTK thread (the module calls `g_idle_add`). On Windows it runs on the audio thread and the tray marshals it with `PostMessage(SWAPP_AUDIO_STATE_MSG = WM_APP + 15)`.

- [ ] **Step 1: Client probe, `audio_linux.c`.** One GLib thread. While `swapp_net_peer` is nonzero:
  - Hold a UDP socket bound to `self_be:0` and `connect()`ed to `peer_be:21423`. Recreate it when the peer or self address changes.
  - Send a probe (empty payload, `seq++`) every second.
  - Ignore `send` errors such as `ECONNREFUSED` from ICMP.

  Pick `stream_id` once at start with `g_random_int()`. Status text:
  - `Audio: waiting for the server` when not connected
  - `Audio: sending to <ip>` when sending
- [ ] **Step 2: Server receiver, `audio_win.c`.** One thread with a UDP socket bound to `INADDR_ANY:21423`. Use `recvfrom` with a 200 ms `SO_RCVTIMEO` so it can check the stop flag. Drop a datagram unless `swapp_net_peer` is connected and the source address equals `peer_be`, then `swapp_audio_packet_parse`. Track the time of the last accepted datagram. Status text:
  - `Audio: waiting for the client` when not connected
  - `Audio: nothing from <ip> over UDP` when connected but silent for `SWAPP_AUDIO_SILENT_MS`, or nothing accepted yet
  - `Audio: receiving from <ip>`
  - `Audio: UDP port 21423 unavailable` when bind failed. This is not terminal for the rest of the app, but it is permanent for audio.

  Call the state callback only when the text changes.
- [ ] **Step 3: Status rows.** Add a third row under the link and monitor rows on both sides:
  - Linux: `g_audio_icon = swapp_row_new(rows, &g_audio_label)`.
  - Windows: `swapp_row_create(&g_audio_row, ..., rows_top + 2 * row_height, ...)`. Grow the frame, and the big buttons' height, from `2 * row_height` to `3 * row_height`.

  Refresh the row in the existing refresh functions from `swapp_audio_ok()` and `swapp_audio_status_text()`.
- [ ] **Step 4: Lifecycle.**
  - Linux: `swapp_audio_start()` next to `swapp_net_start()`. In `swapp_tray_on_quit`, call `swapp_audio_stop(item != NULL)`, with a comment explaining that `NULL` means update handover.
  - Windows: `swapp_audio_start()` after `swapp_net_start()`, and `swapp_audio_stop(0)` after `swapp_net_stop()`.
  - Stop sets a flag and joins with a 500 ms cap. On timeout, detach and move on.
- [ ] **Step 5: CMake.** Add `src/audio_linux.c` and `src/audio_win.c` to their platform source lists.
- [ ] **Step 6: Syntax check.** `x86_64-w64-mingw32-gcc -std=c11 -fsyntax-only -Isrc src/audio_win.c src/tray_win.c` reports no new errors.
- [ ] **Step 7: Commit and push (Stage 1 release).** `feat: probe the audio relay's udp path`. Wait for CI to be green with `gh run watch`.
- [ ] **Step 8: Hardware gate.** Ask the user to update both machines and open the PC's window.
  - `Audio: receiving from 192.168.x.x` means UDP passes. Continue to Stage 2.
  - `nothing from … over UDP` means the firewall blocks it. **Stop and come back to the user.** The spec's fallback (a second TCP connection) needs re-planning, and Stage 2 is written for UDP.

## Stage 2: The relay (one push, after Task 6)

### Task 4: Jitter buffer

**Files:**
- Create: `src/audio_jitter.h`, `src/audio_jitter.c`
- Modify: `tests/audio_tests.c`, `CMakeLists.txt` (add to `swapp` on Windows and to `swapp_audio_tests`)

**Interfaces:**
- Produces:
  ```c
  #define SWAPP_JITTER_SLOTS 64        /* 640 ms of 10 ms frames: the far-jump horizon */
  #define SWAPP_JITTER_MAX_LOST 20     /* consecutive losses (200 ms) before re-priming */
  typedef enum { SWAPP_JITTER_SILENCE, SWAPP_JITTER_PACKET, SWAPP_JITTER_LOST } swapp_jitter_result;
  typedef struct { /* fixed slot ring: per slot seq, len, used flag, payload[SWAPP_AUDIO_MAX_PAYLOAD];
                      stream_id, has_stream, next_seq, priming, lost_run, target_frames */ } swapp_jitter;
  void swapp_jitter_init(swapp_jitter *j, int target_frames);
  void swapp_jitter_push(swapp_jitter *j, uint32_t stream_id, uint32_t seq, const uint8_t *data, size_t len);
  swapp_jitter_result swapp_jitter_pop(swapp_jitter *j, uint8_t *out, size_t *out_len); /* out holds SWAPP_AUDIO_MAX_PAYLOAD */
  int swapp_jitter_depth(const swapp_jitter *j);       /* frames from next_seq to the newest held packet, 0 when empty */
  uint32_t swapp_jitter_stream(const swapp_jitter *j); /* changes when a new sender appears: caller resets its decoder */
  ```
  Rules. Every sequence comparison uses `(int32_t)(a - b)`, so it is wrap-safe.
  - **Push, ignored:** an empty payload (a probe), or `len > SWAPP_AUDIO_MAX_PAYLOAD`.
  - **Push, new stream:** a different `stream_id`, or the first one ever, resets the buffer to that stream with `next_seq = seq`, priming.
  - **Push, too late:** `seq` before `next_seq` is dropped.
  - **Push, far jump:** `seq >= next_seq + SLOTS` resets to priming with `next_seq = seq`.
  - **Push, duplicate:** a `seq` already held is ignored.
  - **Pop while priming:** returns SILENCE until `depth >= target_frames`.
  - **Pop while playing:** returns PACKET for `next_seq`, or LOST if that slot is empty. Either way it advances `next_seq`.
  - **Pop after losses:** after `MAX_LOST` consecutive LOSTs it goes back to priming and returns SILENCE.

- [ ] **Step 1: Write the failing tests.** All use target 4 unless noted.
  ```c
  test_jitter_primes:            push seq 0..2 -> pop == SILENCE; push 3 -> pop == PACKET with seq 0's bytes
  test_jitter_in_order:          push 0..9, pop x10 == PACKET each, payload byte == seq
  test_jitter_reorders:          push 0,1,3,2 -> pops yield payloads 0,1,2,3
  test_jitter_loss_is_lost:      push 0..3,5 -> pops: P,P,P,P,LOST,P
  test_jitter_late_dropped:      prime 0..3, pop twice, push 0 again -> depth unchanged, next pop is seq 2
  test_jitter_duplicate_ignored: push 0,0,1,2,3 -> depth == 4
  test_jitter_new_stream_resets: playing stream A; push stream B seq 500..503 -> pop yields B/500; stream() == B
  test_jitter_far_jump_resyncs:  playing at seq 10; push seq 10+64 -> next pops SILENCE until depth >= 4 from 74
  test_jitter_reprimes_after_gap:prime 0..3, pop 4 -> then 20 pops with nothing pushed: 19x LOST, then SILENCE
  test_jitter_burst_after_gap:   prime 0..3, pop 4x, then 4 pops LOST (seq 4..7 missing), push 8..12 at once -> pops 8..12 PACKET, no reset
  test_jitter_probe_ignored:     push len 0 -> depth == 0, has no stream
  test_jitter_seq_wraps:         prime at 0xFFFFFFFE..0x00000001 -> 4 PACKETs in order
  ```
- [ ] **Step 2: Run, expect failure.** `gcc -std=c11 -Wall -Wextra -Isrc tests/audio_tests.c src/audio_packet.c src/audio_jitter.c -o /tmp/claude-1000/audio_tests` fails with an undefined `swapp_jitter_init`.
- [ ] **Step 3: Implement `src/audio_jitter.c`.** Index slots by `seq % SLOTS`, and check the stored seq on pop.
- [ ] **Step 4: Run, expect `all passed`.**
- [ ] **Step 5: Commit.** `feat: add the audio relay jitter buffer`

### Task 5: Opus build and client capture

**Files:**
- Modify: `CMakeLists.txt`, `.github/workflows/release.yml` (add `libpulse-dev`), `src/audio_linux.c`

**Interfaces:**
- Consumes: `swapp_net_peer`, `swapp_audio_packet_write`, and the `audio.h` constants.
- Produces: `swapp_audio_stop(1)` unloads the sink module; `swapp_audio_stop(0)` leaves it.

- [ ] **Step 1: Opus via FetchContent.** Set these before `FetchContent_MakeAvailable`, each as `CACHE BOOL "" FORCE`: `OPUS_BUILD_PROGRAMS OFF`, `OPUS_BUILD_TESTING OFF`, `OPUS_INSTALL_PKG_CONFIG_MODULE OFF`, `OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF`. Use `GIT_REPOSITORY https://github.com/xiph/opus.git`, `GIT_TAG v1.5.2`, `GIT_SHALLOW TRUE`, then `target_link_libraries(swapp PRIVATE opus)` on both OSes. On Linux, also add `pkg_check_modules(PULSE REQUIRED libpulse)` and wire it in the same way as the other packages. In CI, add `libpulse-dev` to the apt line.
- [ ] **Step 2: Sink setup in `audio_linux.c`.** Replace the probe thread with a `pa_threaded_mainloop` and a context named `Swapp`. When the context is ready:
  - Call `pa_context_get_sink_info_by_name("swapp_relay")`. If it exists, adopt it and remember its `owner_module`.
  - Otherwise, call `pa_context_load_module("module-null-sink", "sink_name=swapp_relay sink_properties='device.description=\"Swapp Audio Relay\"' rate=48000 channels=2")` and remember the returned index.

  Subscribe to `PA_SUBSCRIPTION_MASK_SINK`. On a change event for this sink's index, re-read `info->volume` and store a per-channel linear gain (`pa_sw_volume_to_linear`) under a lock.

  If the context fails or terminates (PipeWire restart), tear everything down and retry every 2 s from a GLib timeout.
- [ ] **Step 3: Capture and send.**
  - Record from `swapp_relay.monitor` as `PA_SAMPLE_FLOAT32LE`, 48000 Hz, 2 channels, with `fragsize = 480 * 2 * 4`, `PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE`.
  - In the read callback, accumulate into a 480-frame buffer and multiply each channel by its gain.
  - Encode with `opus_encode_float` on an encoder from `opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO)` and `OPUS_SET_BITRATE(256000)`.
  - When `swapp_net_peer` is connected, send the datagram on the UDP socket from Task 3 (same bind and connect rules), `seq++`. When not connected, encode nothing and drop the samples.

  Status text:
  - `Audio: relay device unavailable` when no Pulse server or the module failed
  - `Audio: waiting for the server`
  - `Audio: sending to <ip>`
- [ ] **Step 4: Stop.** Lock the mainloop. If `remove_device`, issue `pa_context_unload_module(index)` and wait for the operation for at most 300 ms. Then disconnect and stop the mainloop. The whole stop stays within the 500 ms cap.
- [ ] **Step 5: Local check.** Re-run the unit tests. If libpulse headers are present, run `gcc -fsyntax-only` on `audio_linux.c`; if they are absent, note that CI is the first compile. Do not push yet: Stage 2 ships at the end of Task 6, so the release never carries a sender with no player.
- [ ] **Step 6: Commit.** `feat: capture the relay device and stream it as opus`

### Task 6: Server playback

**Files:**
- Modify: `src/audio_win.c`, `CMakeLists.txt` (`ole32`, `avrt`)

**Interfaces:**
- Consumes: the `swapp_jitter_*` functions (Task 4), Opus, and the receiver from Task 3, which now pushes into a `swapp_jitter` under a `CRITICAL_SECTION`.

- [ ] **Step 1: Render thread.**
  - `CoInitializeEx(NULL, COINIT_MULTITHREADED)` and `AvSetMmThreadCharacteristicsW(L"Pro Audio", &task)`.
  - `#define COBJMACROS` and `#include <initguid.h>` before `<mmdeviceapi.h>`, `<audioclient.h>` and `<functiondiscoverykeys_devpkey.h>`, so the IIDs are defined in C.
  - Take the default endpoint (`eRender, eConsole`) and read `PKEY_Device_FriendlyName` for the status.
  - Format: `WAVEFORMATEXTENSIBLE` float32, 48000 Hz, 2 channels, `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`.
  - Initialize in shared mode with `AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY` and a 20 ms buffer (`200000` hns).
  - `SetEventHandle`, `GetService(IAudioRenderClient)`, `Start`.
- [ ] **Step 2: Feed loop.** `WaitForMultipleObjects` on {render event, stop event, device-change event}.
  - **Render event:** while `bufferFrames - padding >= 480`, `swapp_jitter_pop`, then:
    - PACKET: `opus_decode_float`.
    - LOST: `opus_decode_float(dec, NULL, 0, …)` (PLC).
    - SILENCE: write zeros.
    - If `swapp_jitter_stream` changed since the last pop, first apply `OPUS_RESET_STATE`.
  - **Device change, or any call returning `AUDCLNT_E_DEVICE_INVALIDATED`:** release everything and reopen the default device. With no device, retry every 1 s.
  - **Stop event:** `Stop` and release.
- [ ] **Step 3: Follow the default.** A C `IMMNotificationClient`: a static vtable with all 8 methods, refcount pinned at 1. `OnDefaultDeviceChanged(eRender, eConsole)` sets the device-change event, and the other methods return `S_OK`. Register it on the enumerator, and unregister it before the thread exits.
- [ ] **Step 4: Status.**
  - `Audio: playing on <friendly name>` while datagrams arrive and the stream is open
  - `Audio: no output device` with no device
  - The Task 3 texts otherwise
- [ ] **Step 5: Syntax check** with mingw. Opus headers come from `git clone --depth 1 -b v1.5.2 https://github.com/xiph/opus /tmp/claude-1000/opus`, then `-I/tmp/claude-1000/opus/include`.
- [ ] **Step 6: Commit and push (Stage 2 release).** `feat: play the relayed audio on the default output`. Then `gh run watch` until green, and ctest runs in CI.
- [ ] **Step 7: Hardware gate.** Ask the user to update both machines, select `Swapp Audio Relay` once, and play music.
  - Expected: sound from the RCF, and the laptop's volume keys change the loudness.
  - Power-cycle the RCF: playback resumes within about 1 s.
  - Run `systemctl --user restart pipewire pipewire-pulse` on the laptop: the device returns, and `pactl list sinks short | grep -c swapp_relay` prints `1`.
  - Update via the tray: music never plays from the laptop speakers.
  - Quit from the tray: the device disappears.

  Report any dropouts the user hears. If they hear any, raise `SWAPP_AUDIO_TARGET_MS` in 20 ms steps.

## Stage 3: Drift control (one push, after Task 7)

### Task 7: Rate adjustment, buffered-delay readout, docs

**Files:**
- Modify: `src/audio_jitter.h`, `src/audio_jitter.c`, `tests/audio_tests.c`, `src/audio_win.c`, `CLAUDE.md`

**Interfaces:**
- Produces:
  ```c
  typedef struct { double smoothed_ms; int primed; } swapp_drift;
  void   swapp_drift_reset(swapp_drift *d);
  double swapp_drift_update(swapp_drift *d, double fill_ms, double target_ms); /* playback-rate ratio, called once per 10 ms frame */
  ```
  The controller: on the first update after a reset, `smoothed = fill`. After that, `smoothed += 0.01 * (fill - smoothed)`, which gives about a 1 s time constant at 100 calls per second. It returns `1 + clamp((smoothed - target) * 0.0001, -0.005, +0.005)`, so 10 ms of error gives 0.1% (about 1.7 cents) and the ratio never exceeds ±0.5%.

- [ ] **Step 1: Write the failing tests.**
  ```c
  test_drift_at_target:        reset; 500 updates at fill 40, target 40 -> ratio == 1.0 (exact)
  test_drift_too_full_speeds:  reset; 500 updates at fill 60 -> ratio > 1.0019 && < 1.0021
  test_drift_too_empty_slows:  reset; 500 updates at fill 20 -> ratio < 0.9981 && > 0.9979
  test_drift_clamped:          reset; 500 updates at fill 1000 -> ratio == 1.005
  test_drift_spike_smoothed:   reset; 200 at 40, then one at 140 -> ratio < 1.0002
  ```
- [ ] **Step 2: Run, expect failure** (undefined `swapp_drift_update`).
- [ ] **Step 3: Implement in `src/audio_jitter.c`.**
- [ ] **Step 4: Run, expect `all passed`.**
- [ ] **Step 5: Wire into `audio_win.c`.**
  - Add `AUDCLNT_STREAMFLAGS_RATEADJUST` to Initialize, and `GetService(&IID_IAudioClockAdjustment)`.
  - Per popped frame: `fill_ms = swapp_jitter_depth * 10 + padding * 1000.0 / 48000`. Pass that to `swapp_drift_update` with target `SWAPP_AUDIO_TARGET_MS + 20`, the WASAPI buffer being 20 ms. Every 100th frame, call `SetSampleRate(48000.0f * ratio)`.
  - Reset the drift and set the rate back to 48000 whenever the jitter buffer is priming or the stream changes.
  - If Initialize fails with RATEADJUST, retry without it and append ` (no drift control)` to the status.
  - Append ` · <round(smoothed)> ms buffered` to the `playing on` status.
- [ ] **Step 6: Syntax check** (mingw, as in Task 6).
- [ ] **Step 7: CLAUDE.md.** Under Transport, add a short paragraph covering:
  - Audio is UDP, client to server only, on the same port number. Give the reason: WiFi retransmit stalls on TCP, and `nc`.
  - The server trusts only the connected client's IP.
  - The null sink's monitor already carries the sink volume, so the client applies no gain (a double-applied volume was caught in Task 5).
  - The sink is kept across update handovers so music never falls back to the laptop speakers.
- [ ] **Step 8: Commit and push (Stage 3 release).** `feat: steer playback rate to hold the relay's delay steady`. Then `gh run watch`.
- [ ] **Step 9: Hardware gate.** Play music for an hour.
  - The `ms buffered` readout stays within ±10 ms of target, and no glitches are heard.
  - If the status shows `(no drift control)`, stop and report to the user.
  - Windows shuts down promptly while music is streaming.

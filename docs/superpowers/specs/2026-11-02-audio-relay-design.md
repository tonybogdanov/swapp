# Audio relay: laptop sound to the PC's speakers

## Intent

The RCF TRK PRO1 speakers are connected by USB to the Windows PC (the Swapp
server). The work laptop (the Swapp client) has no way to reach them without
moving the USB cable. SonoBus currently streams the laptop's audio to the PC
and proves the network path works, but it has three problems: it cannot sit
in the tray, it blocks a clean Windows shutdown, and it does not reliably
reconnect after a reboot. Swapp already autostarts on both machines, lives in
the tray and reconnects by itself, so it takes over this job and SonoBus goes
away.

The user listens to music almost all of the time. Latency should be as low
as is practical, but it is not critical: lip-sync is not a requirement.
Uninterrupted, glitch-free playback over a WiFi link matters more than the
last few milliseconds.

## Behaviour

**Client (Linux laptop).** While Swapp runs, it registers an output device
named `Swapp Audio Relay`. The user selects it as the output, the same way as
any other device. Swapp never selects it on its own. Everything the laptop
plays into it is encoded as Opus (48 kHz, stereo, 256 kbps) and sent to the
server. The laptop's own volume control for that device must still work,
meaning the stream carries the volume-adjusted signal. When the link to the
server is down, the device stays present and its sound is discarded.

**Server (Windows PC).** Received audio plays on the current default output
device, which in practice is the RCF. It mixes with the PC's own sound
(shared mode, never exclusive). If the default device changes, or the device
disappears and returns (for example the RCF being power-cycled), playback
follows it with no user action.

**Independent of monitors.** Audio does not care which machine is showing
the monitors. It flows whenever the link is connected, including after a
switch to Windows, when the laptop has lost its screens but is still playing
music.

**Lifecycle.** The relay starts and stops with the link: the client starts
streaming when it connects and stops when the link drops. It recovers on its
own after a dropped link, a WiFi blip, sleep/resume of either machine, or a
restart of either side. Quitting Swapp, or shutting Windows down, must never
hang on audio teardown. That is SonoBus's main failure and this feature has
to avoid it.

## Transport (decided)

Audio travels as **UDP datagrams from client to server**, alongside the
existing TCP control link. The TCP link is not used for audio.

- *Why not the TCP link.* Two reasons. First, binary frames in the text
  stream would break `nc <server> 21423` as a debugging client. Second, a
  single lost packet on WiFi stalls TCP for a retransmit, and a TCP-based
  jitter buffer has to be sized for those stalls. UDP lets a lost packet
  simply be lost: Opus packet-loss concealment covers it, and the buffer can
  stay small.
- *Direction.* Only the client sends. The laptop's firewall drops unsolicited
  inbound traffic, so the server never sends audio-side packets. Any feedback
  the server needs to give, if the builder finds it needs some, goes over the
  TCP link.
- *Addressing.* Datagrams go to the server's address as already discovered
  by the TCP link, on UDP port 21423 (the same number as TCP; the
  port-range rationale in `src/net.h` applies). The client binds its UDP
  socket to its LAN source address for the same VPN reason the TCP sweep
  does.
- *Trust.* The server accepts datagrams only from the IP address of the
  currently connected TCP client, and only while that client is connected.
  Anything else is dropped without being decoded.
- *Windows firewall.* The existing link only proves that inbound **TCP** on
  21423 is allowed. Windows firewall rules are per protocol, so UDP to the
  same port is separate traffic. The prompt Windows shows when a program
  first listens usually creates both a TCP and a UDP rule for that program,
  but this has not been checked on the PC, and Swapp cannot add a rule
  because it has no elevation. Stage 1 below settles it. If UDP turns out to
  be blocked, the fallback is to send audio over a second TCP connection to
  21423. That connection identifies itself with a first line such as `audio`,
  so the control connection and `nc` are unaffected, and it uses a larger
  jitter buffer to cover WiFi retransmit stalls. *(My call.)*

Each datagram carries one Opus packet plus enough header for the receiver to
order packets, detect loss and spot a restarted sender (at minimum a sequence
number, and preferably a per-stream id). The exact layout is the builder's
call. Document it in `src/net.h` alongside the rest of the protocol.

## Audio path

**Client capture.** The laptop runs PipeWire with its PulseAudio
compatibility layer (verified: `pactl info` reports a Pulse server), and the
client must work as an unprivileged user. Approach: use libpulse to load a
`module-null-sink` named for Swapp, with the description `Swapp Audio Relay`,
then record from that sink's monitor source. On quit, unload the module. On
startup, adopt a Swapp null sink already loaded by a previous run (left over
from an update handover or a crash) instead of creating a second one. Unload
the module only when the user chooses Quit; an update handover leaves it
loaded. Removing the sink moves its streams to the laptop's own speakers, so
recreating it would play the user's music out loud for a moment on every
update. *(My call.)* Libpulse is the builder's default. A native
PipeWire implementation is acceptable if it is clearly simpler and meets the
same requirements.

**Codec.** Opus, 48 kHz, stereo, 256 kbps, using the `OPUS_APPLICATION_AUDIO`
(music) mode. The builder picks the frame size; 10 ms is the expected
choice, as a balance between latency and per-packet overhead on WiFi. The
encoder runs continuously while connected, including during silence. Using
DTX to save bandwidth during silence is optional.

**Server playback.** WASAPI shared mode on the default render endpoint,
event-driven. Device changes are tracked with `IMMNotificationClient`.

**Jitter buffer and clock drift.** The laptop's clock and the RCF's clock
will drift apart. Left alone, that drift either drains the buffer (dropouts)
or fills it (latency grows without bound) over a long listening session. The
fix must not cause a periodic audible glitch, so occasionally dropping or
repeating a whole frame is not acceptable. Recommended approach: open the
WASAPI stream with `AUDCLNT_STREAMFLAGS_RATEADJUST` and steer the playback
rate through `IAudioClockAdjustment::SetSampleRate` so that buffer fill
stays near its target. Sizing is the builder's call, aiming for the
smallest buffer that plays music on WiFi without audible dropouts. A
lost packet is concealed with Opus PLC. A long gap, such as the client
stopping, should fall back to silence and re-prime the buffer cleanly
rather than play stale audio.

## Dependencies and build

- **Opus:** build from source with CMake `FetchContent`, pinned to a release
  tag, and statically linked on both platforms. This keeps the
  single-binary property on Windows without needing vcpkg, and the
  configuration is the same on both OSes. *(Builder's call; a system
  `libopus` on Linux is acceptable if it's simpler.)*
- **Linux:** add `libpulse-dev` to the CI build job. `libpulse0` is present
  on any desktop with Pulse or PipeWire-Pulse.
- **Windows:** WASAPI and MMDevice are part of the OS (`ole32`, plus
  whatever else the builder needs to link). No new runtime dependencies.
- New code follows the existing split: one header shared by both builds
  plus `_linux.c` and `_win.c` implementations, with the same
  threading conventions as `net_*.c` (socket and audio work on their own
  threads, UI callbacks marshalled to the UI thread).

## Sleep

The PC is set never to sleep. Swapp may keep it awake while it runs, so
there is no need to release the playback stream during silence to allow
idle sleep. Keeping a WASAPI stream open while the client is connected is
fine. *(User's answer.)*

## Delivery in stages

Every push to `main` replaces the only release, and both machines run it, so
the feature lands in stages. Each stage has to leave the app working as it
does today. *(The user agreed to staging.)*

1. **UDP probe.** The client sends small datagrams over UDP and the server
   shows in its tray whether they arrive. This settles the firewall question
   above before any audio code is written.
2. **Relay.** Adds the null sink, Opus, the UDP stream and WASAPI playback
   with a plain fixed-target jitter buffer.
3. **Drift control.** Adds rate adjustment so that hours of playback stay
   glitch-free with a bounded delay, then device-change following and the
   tray readouts.

The builder may merge or split stages, provided each one ships something
that works.

## UI

Kept minimal. Each tray already shows a greyed link-state readout. Add one
greyed line beneath it for the relay's state. On the client this is
something like "Audio: sending" or "Audio: idle". On the server it is
something like "Audio: playing on Speakers (RCF TRK PRO1)", or "Audio: no
stream". There is no on/off toggle, volume control or settings. *(My call;
the exact wording is the builder's.)*

## Goals

- Laptop audio plays through the RCF with no manual steps after the user
  has selected `Swapp Audio Relay` once.
- Survives reboots, sleep, WiFi blips and RCF power-cycles, reconnecting
  without the user doing anything.
- Clean, prompt shutdown on both sides.
- Hours of continuous music with no periodic glitches and no growing delay.

## Non-goals

- PC to laptop audio, microphone, or any reverse direction.
- Automatically making the relay the laptop's default output.
- Lip-sync-grade latency, encryption, or authentication beyond the
  "connected client's IP only" rule.
- Configurable codec settings, port, or device. These are fixed values.
- Choosing a non-default output device on the server.

## Anti-goals

These would make the feature a failure even if it technically works:

- Anything that delays or blocks Windows shutdown or Swapp quitting.
- A leftover or duplicate `Swapp Audio Relay` device on the laptop after a
  crash or update.
- Breaking `nc <server> 21423` as a working debugging client.
- Audio traffic leaking onto the corporate VPN.

## Verification

There is no local build. Every push to `main` builds through CI, so proof
comes from running the release binaries on the two real machines: select the
relay on the laptop, play music, and confirm it comes out of the RCF; drop
WiFi and confirm it recovers; quit both sides and confirm they exit
promptly; shut Windows down while streaming; leave music playing for an
hour and confirm the delay has not grown and there are no glitches. Pieces
that can be tested without the hardware, such as the jitter buffer's
reorder/loss/drift logic and the datagram header parsing, should be.

## Docs

Once it is built, add the non-obvious rationale to `CLAUDE.md` under
Transport: why audio uses UDP while control uses TCP, why only the client
sends, and the firewall note.

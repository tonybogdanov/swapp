# Swapp

Cross-platform desktop app for Windows and Linux.

## Build and run

Two scripts per OS, named `z_*` only so they sort below the source
directories:

- `z_install-deps.sh` / `z_install-deps.ps1` -- installs the toolchain and
  system packages. Needs sudo (Linux) or an elevated shell (Windows). Run once
  per machine; agents should not run these.
- `z_run.sh` / `z_run.ps1` -- kills the running instance, builds, starts the
  new binary. No elevation: the build itself never needs it.

Each time the agent implements a change, it runs the `z_run` script for the
current OS. It is incremental, so it is cheap to run on every change; wipe the
build directory by hand if CMake's cache ever needs resetting.

## Modes

Swapp has two modes, fixed at compile time by the target OS (`src/mode.h`):

- **server** -- Windows builds only
- **client** -- Linux builds only

The mode is not selectable at runtime: server functionality is meaningless on
Linux and client functionality on Windows, so each build carries exactly one.

The server listens on a network socket for commands. The client connects to it
and issues them. All DDC/CI traffic and all state -- the monitor cache, the
input role assignments, the switching -- live on the server; the client owns
none of it and can only read or modify it through the server.

This is deliberately centralized rather than peer-to-peer:

- Two applications driving the same monitors over DDC/CI independently is
  unreliable. One owner avoids the races.
- The machine being switched *away from* has no dependable way to notice that
  it lost the monitors, so it cannot keep a local cache honest. Only the side
  that issues the switch knows what really happened, so that side keeps the
  state.

Windows is the server because it is the machine wired to the monitors
directly (see `src/monitors_win.c`).

### Transport

TCP on port 21423, one connection, newline-delimited plain text
(`src/net.h`). Chosen over HTTP because the server has to *push* state
changes the client never asked for, and over a binary framing because
`nc <server> 21423` has to stay a working debugging client. The port must
stay below 32768: anything in the ephemeral ranges (Windows 49152+, Linux
32768+) can be handed to some other app's outbound socket -- 51423 was, to
Chrome -- and the server's bind then fails.

The **client dials, the server listens**, even though the client is the one
with nothing to offer: the Linux machine is a locked-down work laptop that
drops all unsolicited inbound traffic, and the Windows machine is personally
owned. Outbound connects, and the replies on that same flow, are the only
thing that reliably crosses.

For the same reason there is no UDP discovery protocol: a broadcast probe's
reply arrives from the server's unicast address rather than the broadcast
address it was sent to, so connection tracking doesn't match it and the
client's firewall drops it. Instead the client finds the server by connecting:
last known address first (cached in `~/.cache/swapp/last-server`), then a
batched sweep of the local /24. Only `192.168.x` interfaces are swept and
point-to-point ones are skipped, so probes never enter the corporate VPN, and
every outbound socket is bound to the LAN source address so VPN routes can't
pull it onto `tun0`.

Both sides ping every 2s and drop the peer after 6s of silence. Silence is
the only disconnect signal that works: the machine being switched away from
never gets to send a goodbye, and its TCP socket can stay open for minutes
after. A dropped link sends the client back to searching; a failed bind on
the server is terminal and surfaced as an error, since a second server would
be the split-brain the design exists to prevent. A second client is refused
with `err busy` rather than queued.

Each side shows the link state in its tray (hover text, and a greyed readout
at the top of the menu).

The client does nothing without the server -- it holds no state of its own --
but the server is whole on its own. It scans at startup, keeps the cache and
serves its own buttons with no client ever connecting; a client arriving is
served whatever is already there, and one leaving takes nothing with it. The
link gates exactly one thing: switching *to Linux* -- the button, and a
single cell from the table menu alike -- which would hand a screen to a
machine that is not there, with nobody to hand it back. The
way back -- switch-all-to-Windows, and F16 -- stays live precisely because it
is the way back.

Startup is otherwise inert -- the old monitor window and the hotplug watch
are commented out in `src/tray_linux.c` and `src/tray_win.c`, waiting on this
split.

F16 means "bring both monitors to *this* machine": on the server it is the
Switch-all-to-Windows button, on the client Switch-all-to-Linux, both under
the same guard (settled server, every monitor fully assigned). So whichever
machine the keyboard is plugged into, F16 takes the screens there. It is a
double tap (second press within 1s; holding doesn't count) so a stray press
can't take both screens away.

### Topology is assigned, not detected

Which input of each monitor is cabled to which machine is set by the user
(table right-click: Assign to Windows / Linux) and persisted on the server.
Detecting it was tried and is wrong by construction: DDC/CI VCP 0x60 reports
the input a monitor is *showing*, and a monitor showing the other machine
still answers over this machine's cable with that other input. Neither side
can tell "my input" from "the active input", so don't reintroduce inference.

## Notes

Don't document requested functionality here when it can be inferred from the
code alone. Record only decisions, constraints and rationale the code doesn't
reveal.

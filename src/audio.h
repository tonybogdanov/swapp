#ifndef SWAPP_AUDIO_H
#define SWAPP_AUDIO_H

#include <stddef.h>

/* The audio relay: laptop audio played on the Windows PC. The client (Linux)
 * sends UDP datagrams, the server (Windows) receives them; the wire format is
 * in audio_packet.h and the reasoning in net.h. Each platform implements this
 * header in audio_linux.c / audio_win.c, the same split as net.h.
 *
 * The client records the monitor of its "Swapp Audio Relay" sink, encodes it
 * as Opus and sends one 10 ms frame per datagram. The server receives them,
 * reorders them in a jitter buffer (audio_jitter.h), decodes them and plays
 * them on its default output device, following the default when it changes. */
#define SWAPP_AUDIO_RATE 48000
#define SWAPP_AUDIO_CHANNELS 2
#define SWAPP_AUDIO_BITRATE 256000
#define SWAPP_AUDIO_FRAME 480      /* samples per channel per datagram: 10 ms */
#define SWAPP_AUDIO_TARGET_MS 120  /* jitter-buffer target: WiFi stalls up to this long play cleanly */
/* Room for the longest status text swapp_audio_status_text can produce. */
#define SWAPP_AUDIO_STATUS_MAX 192
#define SWAPP_AUDIO_SILENT_MS 3000 /* server: connected client, no datagram for this long */

/* Starts the audio threads. Returns immediately. */
void swapp_audio_start(void);

/* Stops the threads, waiting at most 500 ms for all of them together; a
 * thread that does not finish in time is left behind rather than blocking
 * shutdown. remove_device matters only on the client. */
void swapp_audio_stop(int remove_device);

/* Nonzero while the relay is working (client capturing and sending, server
 * receiving and playing); drives the row icon, check versus spinner. */
int swapp_audio_ok(void);

/* Nonzero once the relay has failed for good (the server's UDP port is taken);
 * the row shows an error rather than a spinner. The client has no such state. */
int swapp_audio_failed(void);

/* A one-line status for the UI, written into buf. */
void swapp_audio_status_text(char *buf, size_t buf_size);

/* Registers a function called whenever the status text changes, so the tray
 * can refresh. Same contract as swapp_net_set_state_callback: on Linux it runs
 * on the GTK thread, on Windows on the audio thread and the tray marshals it
 * with PostMessage. */
void swapp_audio_set_state_callback(void (*callback)(void));

#endif

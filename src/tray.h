#ifndef SWAPP_TRAY_H
#define SWAPP_TRAY_H

/* Registers a tray icon with the given tooltip and blocks until the user
 * right-clicks it and chooses "Quit" from the context menu. */
void swapp_tray_run(const char *tooltip);

/* Asks an already-running instance to quit, as if its Quit had been
 * clicked, and waits (a few seconds at most) for it to exit, so its binary
 * can be replaced. Returns zero if no instance was running. */
int swapp_tray_stop_running(void);

#endif

#ifndef SWAPP_TRAY_H
#define SWAPP_TRAY_H

/* Registers a tray icon with the given tooltip and blocks until the user
 * right-clicks it and chooses "Quit" from the context menu. */
void swapp_tray_run(const char *tooltip);

#endif

#ifndef SWAPP_AUTOSTART_H
#define SWAPP_AUTOSTART_H

/* Starting with the user's session, per user, pointing at the running
 * binary -- which is the installed copy (install.h), so the entry survives
 * the download it came from being deleted.
 *
 *   Windows  HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "Swapp"
 *   Linux    ~/.config/autostart/swapp.desktop (XDG autostart)
 */

/* Nonzero when an autostart entry exists. */
int swapp_autostart_enabled(void);

/* Adds or removes the entry. Nonzero on success. */
int swapp_autostart_set(int enabled);

#endif

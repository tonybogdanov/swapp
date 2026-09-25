#ifndef SWAPP_INSTALL_H
#define SWAPP_INSTALL_H

/* A release is a single binary run from wherever it was downloaded, so the
 * first run installs it: the binary copies itself into a fixed per-user
 * location and relaunches from there. Per-user because neither machine
 * gives the app elevation -- Windows runs it unelevated and the Linux laptop
 * is locked down:
 *
 *   Windows  %LOCALAPPDATA%\Programs\swapp\swapp.exe  (FOLDERID_UserProgramFiles)
 *   Linux    ~/.local/bin/swapp
 *
 * If the installed copy is identical, it is simply started. If it differs,
 * this binary is taken as an update: the running copy is stopped, the
 * installed one overwritten, and the new one started. Running a download
 * is therefore also how the app is updated.
 *
 * A binary downloaded by "Check for updates" runs with SWAPP_ARG_UPDATE
 * (update.h); it then starts the installed copy with SWAPP_ARG_CLEANUP and
 * its own path, and the installed copy deletes the download.
 *
 * Returns nonzero when the installed copy has been started and this process
 * should exit; zero when this process is the installed copy, or installing
 * failed and it should just run from where it is. */
int swapp_install_redirect(int argc, char **argv);

#endif

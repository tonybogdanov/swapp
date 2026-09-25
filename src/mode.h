#ifndef SWAPP_MODE_H
#define SWAPP_MODE_H

/* Swapp runs in one of two modes, fixed at compile time by the target OS:
 * the Windows build is the server, the Linux build is the client. The mode
 * is a label for the role each machine plays in the pair; the two builds
 * still behave the same locally (enumerate monitors, assign roles, switch
 * on F16). Nothing is exchanged between them yet. */
typedef enum {
    SWAPP_MODE_CLIENT = 0,
    SWAPP_MODE_SERVER = 1,
} swapp_mode;

#ifdef _WIN32
#define SWAPP_MODE_SELF SWAPP_MODE_SERVER
#define SWAPP_MODE_NAME "server"
#else
#define SWAPP_MODE_SELF SWAPP_MODE_CLIENT
#define SWAPP_MODE_NAME "client"
#endif

#endif

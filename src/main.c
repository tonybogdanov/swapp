#include "install.h"
#include "mode.h"
#include "tray.h"

int main(int argc, char **argv) {
    if (swapp_install_redirect(argc, argv)) {
        return 0;
    }
    swapp_tray_run("Swapp (" SWAPP_MODE_NAME ")");
    return 0;
}

#include "install.h"
#include "mode.h"
#include "tray.h"

int main(void) {
    if (swapp_install_redirect()) {
        return 0;
    }
    swapp_tray_run("Swapp (" SWAPP_MODE_NAME ")");
    return 0;
}

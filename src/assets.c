#include "assets.h"

#include <string.h>

const swapp_asset *swapp_asset_find(const char *name) {
    for (size_t i = 0; i < swapp_asset_count; i++) {
        if (strcmp(swapp_assets[i].name, name) == 0) {
            return &swapp_assets[i];
        }
    }
    return NULL;
}

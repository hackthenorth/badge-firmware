#pragma once

#include "app.h"

const lv_image_dsc_t *pal_image(uint8_t id);
int pal_unlocked_count();

// Shared QR customization page. PAL IDs remain stable; rows group Ginny,
// Patch and Vinyl. B cancels; A saves an unlocked PAL (or None) and closes.
class PalPicker {
public:
    void show(lv_obj_t *root);
    bool on_button(ButtonEvent event);
private:
    void render();
    lv_obj_t *root_ = nullptr;
    int focus_ = 9;
    int column_ = 0;
    const char *error_ = nullptr;
};

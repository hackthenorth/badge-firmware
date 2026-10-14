#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void hal_display_init(void);
lv_obj_t *hal_display_root(void);

// The LVGL display created in hal_display_init(). NULL before init.
// Needed by anything that has to drive a refresh or tap the flush path
// directly rather than just parenting widgets under hal_display_root().
lv_display_t *hal_display_lv(void);

// Paint all pending LVGL content while the panel is hidden, then turn it on.
// The SPI panel driver drains the queued pixel transfers before sending the
// display-on command, so the first visible frame is complete.
void hal_display_show(void);

#ifdef __cplusplus
}
#endif

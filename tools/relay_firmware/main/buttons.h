#pragma once

#include <stdbool.h>
#include <stdint.h>

// On-board editor (same 74HC165 layout as the badge): Up opens the
// station-ID editor, Down the STA-MAC editor; Left/Right adjust. 8 s idle
// exits back to the status screen. The station index and MAC persist in NVS
// across reboots; a newly-set MAC takes effect after a reboot.
void buttons_start(void);

// Current station slug, e.g. "mentor-cafe".
const char *box_id_current(void);

// True with the NVS-configured STA-MAC override when one is set; false
// means chip/CONFIG default. Override uses the event's 24:0A:C4:C8:46
// prefix and the selected 0xA0..0xB3 last byte.
bool box_mac_override(uint8_t out_mac[6]);

// Chip efuse STA MAC.
void box_base_mac(uint8_t out_mac[6]);

// "aa:bb:cc:dd:ee:ff" into an 18-byte buffer.
void box_mac_str(const uint8_t mac[6], char out18[18]);

// Called (from the button task) when exiting edit mode, e.g. to repaint.
void buttons_set_changed_cb(void (*cb)(void));

// Implemented by main.c: re-broadcast the beacon when idle, without
// touching the screen.
void relay_beacon_refresh(void);

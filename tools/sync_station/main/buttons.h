#pragma once

#include <stdbool.h>
#include <stdint.h>

// On-board editor (same 74HC165 layout as the badge): Up opens the
// station-ID editor; Left/Right adjust between sync-01/sync-02. 8 s idle
// exits back to the status screen. The selection persists in NVS across
// reboots.
void buttons_start(void);

// Current station slug, "sync-01" or "sync-02".
const char *box_id_current(void);

// STA-MAC derived from the station id: sync-01 ends in B2, sync-02 in B3
// (event prefix 24:0A:C4:C8:46). Always configured; needs a reboot to apply
// after an ID change, like the relay's editor MAC.
bool box_mac_override(uint8_t out_mac[6]);

// "aa:bb:cc:dd:ee:ff" into an 18-byte buffer.
void box_mac_str(const uint8_t mac[6], char out18[18]);

// Called (from the button task) when exiting edit mode, e.g. to repaint.
void buttons_set_changed_cb(void (*cb)(void));

// Implemented by main.c: re-broadcast the idle beacon when no session is
// live, without touching the screen.
void sync_beacon_refresh(void);

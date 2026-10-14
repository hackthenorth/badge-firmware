#pragma once

// Connectionless BLE link for the relay, matching the badge's main/hal/hal_radio.
//
// This is a deliberately trimmed copy of that file rather than a shared
// component: the relay is a separate ESP-IDF project and can't include from the
// badge's main/. The parts that had to stay identical are the company ID
// (0xFFFF), the AD wrapper layout, and the extended-advertising parameters —
// change any of those on one side and the two stop seeing each other.
//
// What's different from the badge version, on purpose:
//   - No disable/teardown path. The relay brings NimBLE up once and leaves it.
//   - Scan runs at ~100% duty instead of the badge's 20%. The relay is mains
//     powered and its whole job is to not miss frames.

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// See bbx_proto.h: the AD blob must fit one HCI command (251) while
// advertising (251) AND one HCI advertising report at the peer (229), and the
// manufacturer-data wrapper costs 4 bytes. The report limit binds.
#define BLE_LINK_MAX_PAYLOAD 225

// Called on the NimBLE host task for every 0xFFFF manufacturer-data frame we
// hear. Keep it short and non-blocking — it must not do network I/O.
typedef void (*ble_link_rx_cb_t)(const uint8_t *data, size_t len);

esp_err_t ble_link_init(ble_link_rx_cb_t cb);

// Replace the advertised payload. Advertising is continuous, so whatever is set
// here keeps repeating every ~200 ms until it's replaced.
esp_err_t ble_link_set_payload(const void *data, size_t len);   // len <= 225

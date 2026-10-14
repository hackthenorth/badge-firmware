#pragma once

// Connectionless BLE link for the sync station, matching the badge's main/hal/hal_radio.
//
// This is a deliberately trimmed copy of that file rather than a shared
// component: the station is a separate ESP-IDF project and can't include from the
// badge's main/. The parts that had to stay identical are the company ID
// (0xFFFF), the AD wrapper layout, and the extended-advertising parameters —
// change any of those on one side and the two stop seeing each other.
//
// What's different from the badge version, on purpose:
//   - No disable/teardown path. The station brings NimBLE up once and leaves it.
//   - Scan runs at ~100% duty instead of the badge's 20%. The station is mains
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
// here keeps repeating until it's replaced — it is a register, not a queue.
esp_err_t ble_link_set_payload(const void *data, size_t len);   // len <= 225

// Air-traffic counters for the status screen: every extended-advertising
// report the scan sees, and how many carried our 0xFFFF manufacturer field.
// Told apart from a working link by whether mfg moves while a badge syncs.
void ble_link_air_counts(uint32_t *all_out, uint32_t *mfg_out);

// Retune the advertising interval, in milliseconds. Clamped to the BLE-legal
// 20 ms .. 10.24 s.
//
// A transfer wants this low: at the idle 200 ms the station's STAT and GRANT
// frames take ~200 ms each to reach a badge, which dominates every round trip.
// Idle wants it high, because a beacon repeating 33x a second is pure noise on
// a channel every badge in the room is scanning.
//
// Stops advertising, reconfigures, restores the current payload and restarts,
// so the caller does not have to re-send what was already up.
esp_err_t ble_link_set_adv_interval_ms(uint32_t min_ms, uint32_t max_ms);

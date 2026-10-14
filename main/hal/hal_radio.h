#pragma once

#include "esp_err.h"
#include <cstddef>
#include <cstdint>
#include <functional>

// Radio (BLE 5 extended advertising, NimBLE host) is OFF by default. Apps
// that need it must call hal_radio_enable() in on_enter(). The app registry
// force-disables radio on every app switch, so apps don't need to disable
// it on exit.
//
// First call lazily initializes the controller + NimBLE host, once per boot;
// the stack is never torn down (with NimBLE static allocation,
// nimble_port_deinit() is a one-way door — re-init fails — so disable() must
// not call it). While enabled we run as both broadcaster (continuous extended
// adv at ~200 ms interval) and observer (passive ext scan).
esp_err_t hal_radio_enable();
// Read-only USB diagnostics: last startup error, controller state and heap.
// Does not initialize the radio, enable scanning, or transmit a payload.
void hal_radio_debug(char *out, size_t capacity);
// Initialize/check only the BLE controller and host. Starts no advertisements
// or scan, and never loads or sends an app payload. Leaves the host ready.
esp_err_t hal_radio_probe();

// True while the stack is up (between enable and disable). The app registry
// samples this on app exit to decide whether leaving for the launcher should
// reboot for a clean heap instead of switching directly.
bool hal_radio_is_enabled();

// Stops adv + scan so there is no further RF activity. No-op if not enabled.
//
// Non-blocking: a few HCI commands, safe to call under lvgl_port_lock (which
// is where the app registry calls it). The NimBLE stack stays up — see above
// for why it must — and the next enable() just restarts adv/scan.
void hal_radio_disable();

// Largest payload hal_radio_send() will accept.
//
// Two separate HCI ceilings apply, and the smaller one wins:
//
//   TX, 251 bytes. While an instance is advertising, NimBLE requires the whole
//   AD blob to fit one HCI LE Set Extended Advertising Data command (BT 5.0
//   Vol 2, Part E, 7.8.54) and rejects anything larger with BLE_HS_EINVAL —
//   ble_gap_ext_adv_set_data_validate() in ble_gap.c.
//
//   RX, 229 bytes. A scanner delivers advertising data to its host in HCI LE
//   Extended Advertising Report events. That event's Parameter_Total_Length is
//   a single byte, and the per-report fixed fields cost 24 of it plus 2 for the
//   subevent header, so at most 255 - 26 = 229 bytes of data fit one report.
//   Anything larger is split across reports with data_status INCOMPLETE, and
//   NimBLE does NOT reassemble — it hands each fragment to the app as its own
//   BLE_GAP_EVENT_EXT_DISC.
//
// RX binds. The AD wrapper spends 4 bytes on
// [length][type=0xFF][company_lo][company_hi], leaving 225.
//
// Both failure modes read as bad RF rather than as a size problem, which is
// what makes them expensive: over the TX limit nothing is transmitted, and
// over the RX limit it is transmitted but arrives as fragments that no
// receiver reassembles. In both cases the frames that vanish are the big ones,
// exactly as genuine path loss would behave.
static constexpr size_t kHalRadioMaxPayload = 225;

// Update the broadcast payload (<=kHalRadioMaxPayload bytes; wrapped in BT
// manufacturer-specific data). Continuous adv means receivers pick up the new
// payload on the next interval (~200 ms). Returns ESP_ERR_INVALID_STATE if
// disabled, ESP_ERR_INVALID_SIZE if too long.
//
// NOTE: this is not a discrete transmit. It overwrites a *persistent*
// advertisement that keeps repeating until overwritten, paused, or the radio
// is disabled. Anything privacy-sensitive must be followed by
// hal_radio_adv_pause() — see app_badge_bump.
esp_err_t hal_radio_send(const void *data, size_t len);

// Stop / restart advertising without tearing down the BLE stack.
//
// This is the cheap counterpart to hal_radio_disable(): a single non-blocking
// HCI command. Safe to call from an LVGL tick. Scanning is unaffected — a
// paused badge still receives. Both are no-ops if the radio is disabled or
// already in the requested state.
esp_err_t hal_radio_adv_pause();
esp_err_t hal_radio_adv_resume();
bool      hal_radio_is_advertising();

// Retune the advertising interval. Shorter = more advertising events per
// second = a short broadcast burst is far likelier to be heard, at the cost
// of airtime and power. Defaults to 200-220 ms.
//
// A burst shorter than the adv interval emits roughly ONE advertising event,
// which a peer scanning at partial duty will usually miss. Any app that
// broadcasts for less than ~500 ms must drop this first (30 ms is a good
// working value) or its transmits will fail silently.
//
// Clamped to the BLE-legal 20 ms .. 10.24 s. Takes effect immediately;
// briefly stops and restarts adv, preserving the current payload. Safe to
// call before hal_radio_enable() — the value is remembered.
esp_err_t hal_radio_set_adv_interval_ms(uint32_t min_ms, uint32_t max_ms);

// Retune the scan duty cycle. Defaults to a 30 ms window per 150 ms interval
// (20% duty), which means ~80% of any single advertising event is missed.
// Pass equal values for continuous scanning (100% duty).
//
// Clamped to the BLE-legal 2.5 ms .. 10.24 s, and window is clamped to
// interval. Takes effect immediately; briefly cancels and restarts the scan.
esp_err_t hal_radio_set_scan_duty_ms(uint32_t interval_ms, uint32_t window_ms);

// Restore the compiled-in defaults for both of the above. Call on app exit if
// the app retuned them; the registry's force-disable also resets them.
void hal_radio_reset_timings();

// Callback fires from the NimBLE host task on each matching scan result.
// `mac` is the peer's BLE address (stable static random per-device). Keep
// work tiny; queue and process in your app's on_tick().
//
// `rssi` is the received signal strength in dBm, straight from the scan
// report. **127 is the "unavailable" sentinel** — check for it before using
// the value, or a missing reading reads as an extremely strong one.
using RadioRecvCb = std::function<void(const uint8_t mac[6], int8_t rssi,
                                       const uint8_t *data, size_t len)>;
void hal_radio_set_recv_cb(RadioRecvCb cb);

// Read this badge's BLE static random address (used as peer-id). Derived from
// the chip's WiFi MAC, so it is stable across reboots and is answered correctly
// even with the radio powered down — callers building a payload before
// hal_radio_enable() get the real address, not zeros.
void hal_radio_local_mac(uint8_t out[6]);

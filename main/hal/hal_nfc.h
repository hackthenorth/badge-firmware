#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"

struct NfcCard {
    uint8_t uid[10];
    size_t uid_len;
    uint8_t sak;
    uint16_t atqa;
};

// NFC is power-hungry and is OFF by default. Apps that need it must call
// hal_nfc_enable() in on_enter(). The app registry force-disables NFC on
// every app switch, so apps don't need to disable it on exit.
//
// While enabled the antenna field is duty-cycled in software (~750 ms on /
// ~750 ms off): the MFRC522 has no hardware low-power card detect, so this is
// what keeps the 13.56 MHz field from drawing full current continuously. The
// cycle is driven by hal_nfc_last_card(), which every consumer already polls;
// a latched card holds the field on until cleared so hal_nfc_read_text()
// never races an off-transition.
//
// First call lazily creates the rc522 driver. Subsequent calls resume polling
// after a hal_nfc_disable(). Safe to call repeatedly.
esp_err_t hal_nfc_enable();

// Stops the rc522 polling loop. No-op if never enabled.
void hal_nfc_disable();

// True while the field is up (between enable and disable). The app registry
// samples this on app exit to decide whether leaving for the launcher should
// reboot for a clean heap instead of switching directly.
bool hal_nfc_is_enabled();

// Appends one timestamped line to /littlefs/nfc_journal.txt (flushed, bounded
// at 4 KB). Survives the power-cycle needed after a battery crash, so the
// last line shows which NFC stage died. Read with the console `cat` command.
void hal_nfc_journal(const char *stage);

// Drops the antenna field immediately but keeps scanning enabled: the duty
// cycle resumes automatically on the next hal_nfc_last_card() poll. Call
// after hal_nfc_read_text() + hal_nfc_clear_last_card() so LED feedback never
// overlaps the energized field (peak-current win on batteries). The latch
// must be cleared first — duty_tick() re-energizes while a card is latched.
void hal_nfc_field_down();

// Hello-world handshake with the rc522 reader IC: reads its VersionReg and
// confirms the chip is responding on the I2C bus, independent of whether any
// card is present. Lazily inits the driver (does not start polling). On
// success returns ESP_OK and, if version_out is non-null, the raw version byte
// (0x91/0x92 for a genuine MFRC522). Returns ESP_ERR_NOT_FOUND if the bus reads
// back all-zero/all-one (no chip), or the underlying I2C error.
esp_err_t hal_nfc_probe(uint8_t *version_out);

// Returns true if a card has been seen since the last clear; fills *out.
bool hal_nfc_last_card(NfcCard *out);
void hal_nfc_clear_last_card();

// Reads the first NDEF Text record (TNF=Well Known, type "T") from the card
// most recently seen. Writes a NUL-terminated UTF-8 string into out.
//
// Targets NTAG21x / Ultralight family. Returns ESP_ERR_NOT_FOUND if no card,
// no NDEF message, or no text record. Returns the underlying read error if
// the card is removed or unreadable.
esp_err_t hal_nfc_read_text(char *out, size_t out_sz);

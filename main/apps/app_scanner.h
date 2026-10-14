#pragma once

class App;

App *app_scanner_instance();

// Transfers an already-read NFC text payload into Scanner so switching from
// another NFC-aware app does not require a second tap.
void app_scanner_prepare_payload(const char *payload);

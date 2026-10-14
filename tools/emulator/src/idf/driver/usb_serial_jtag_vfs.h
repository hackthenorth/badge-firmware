#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LINE_ENDINGS_CRLF = 0,
    ESP_LINE_ENDINGS_CR,
    ESP_LINE_ENDINGS_LF,
} esp_line_endings_t;

// Payload bytes reach stdin untranslated in the emulator.
static inline void usb_serial_jtag_vfs_set_rx_line_endings(esp_line_endings_t mode)
{
    (void)mode;
}

#ifdef __cplusplus
}
#endif

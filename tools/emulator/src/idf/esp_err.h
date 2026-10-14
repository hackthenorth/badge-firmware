#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                  -1
#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_TIMEOUT           0x107
#define ESP_ERR_INVALID_RESPONSE  0x108
#define ESP_ERR_INVALID_CRC       0x109
#define ESP_ERR_INVALID_VERSION   0x10A
#define ESP_ERR_INVALID_MAC       0x10B
#define ESP_ERR_NOT_FINISHED      0x10C
#define ESP_ERR_NOT_ALLOWED       0x10D

const char *esp_err_to_name(esp_err_t code);

// Same contract as the firmware: a failed check is fatal (panic + reboot).
void emu_error_check_failed(esp_err_t code, const char *file, int line,
                            const char *expr) __attribute__((noreturn));

#define ESP_ERROR_CHECK(x)                                                    \
    do {                                                                      \
        esp_err_t emu_err_rc_ = (x);                                          \
        if (emu_err_rc_ != ESP_OK) {                                          \
            emu_error_check_failed(emu_err_rc_, __FILE__, __LINE__, #x);      \
        }                                                                     \
    } while (0)

#ifdef __cplusplus
}
#endif

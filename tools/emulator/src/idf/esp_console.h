#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*esp_console_cmd_func_t)(int argc, char **argv);

typedef struct {
    const char *command;
    const char *help;
    const char *hint;
    esp_console_cmd_func_t func;
    void *argtable;
} esp_console_cmd_t;

typedef struct {
    uint32_t max_history_len;
    const char *history_save_path;
    uint32_t task_stack_size;
    uint32_t task_priority;
    const char *prompt;
    size_t max_cmdline_length;
} esp_console_repl_config_t;

#define ESP_CONSOLE_REPL_CONFIG_DEFAULT()                                     \
    {                                                                         \
        .max_history_len = 32, .history_save_path = NULL,                     \
        .task_stack_size = 4096, .task_priority = 2, .prompt = NULL,          \
        .max_cmdline_length = 0,                                              \
    }

typedef struct {
    int reserved;
} esp_console_dev_usb_serial_jtag_config_t;

#define ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT() { .reserved = 0 }

typedef struct esp_console_repl_s esp_console_repl_t;

// The page's console panel stands in for the USB-Serial-JTAG REPL: it hands
// whole lines to the registered commands, so there is no linenoise loop.
esp_err_t esp_console_new_repl_usb_serial_jtag(
    const esp_console_dev_usb_serial_jtag_config_t *dev_config,
    const esp_console_repl_config_t *repl_config, esp_console_repl_t **ret_repl);
esp_err_t esp_console_register_help_command(void);
esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd);
esp_err_t esp_console_start_repl(esp_console_repl_t *repl);

#ifdef __cplusplus
}
#endif

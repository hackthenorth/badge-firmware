#pragma once

// Starts an esp_console REPL on USB Serial JTAG and registers the badge's
// dev / file-transport commands. Must be called after hal_buttons_init,
// hal_fs_init, and app_registry_start.
void hal_console_start();

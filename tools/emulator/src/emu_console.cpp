// esp_console for the browser: the firmware registers its USB console
// commands as usual (hal_console.cpp is compiled unmodified) and the page's
// console panel feeds whole lines to emu_console_exec().

#include "emu.h"

#include "esp_console.h"

#include <emscripten.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Command {
    std::string name;
    std::string help;
    esp_console_cmd_func_t func;
};

std::vector<Command> &commands()
{
    static std::vector<Command> list; // construct-on-first-use
    return list;
}

int cmd_help(int, char **)
{
    for (const Command &c : commands()) {
        printf("%-18s %s\n", c.name.c_str(), c.help.c_str());
    }
    return 0;
}

// Same rules as esp_console_split_argv: whitespace separates, double quotes
// group, a backslash escapes the next character.
size_t split_argv(char *line, char **argv, size_t argv_cap)
{
    size_t argc = 0;
    char *out = line;
    char *in = line;
    while (*in && argc + 1 < argv_cap) {
        while (*in == ' ' || *in == '\t') ++in;
        if (!*in) break;
        argv[argc++] = out;
        bool quoted = false;
        while (*in) {
            const char c = *in;
            if (c == '\\' && in[1]) {
                *out++ = in[1];
                in += 2;
            } else if (c == '"') {
                quoted = !quoted;
                ++in;
            } else if (!quoted && (c == ' ' || c == '\t')) {
                break;
            } else {
                *out++ = c;
                ++in;
            }
        }
        if (*in) ++in;
        *out++ = '\0';
    }
    argv[argc] = nullptr;
    return argc;
}

} // namespace

struct esp_console_repl_s {
    int unused;
};

extern "C" esp_err_t esp_console_new_repl_usb_serial_jtag(
    const esp_console_dev_usb_serial_jtag_config_t *,
    const esp_console_repl_config_t *, esp_console_repl_t **ret_repl)
{
    static esp_console_repl_s repl;
    if (ret_repl) *ret_repl = &repl;
    return ESP_OK;
}

extern "C" esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd)
{
    if (!cmd || !cmd->command || !cmd->func) return ESP_ERR_INVALID_ARG;
    for (Command &existing : commands()) {
        if (existing.name == cmd->command) {
            existing.help = cmd->help ? cmd->help : "";
            existing.func = cmd->func;
            return ESP_OK;
        }
    }
    commands().push_back({ cmd->command, cmd->help ? cmd->help : "", cmd->func });
    return ESP_OK;
}

extern "C" esp_err_t esp_console_register_help_command(void)
{
    const esp_console_cmd_t help = {
        .command = "help",
        .help = "Print the list of registered commands",
        .hint = nullptr,
        .func = &cmd_help,
        .argtable = nullptr,
    };
    return esp_console_cmd_register(&help);
}

extern "C" esp_err_t esp_console_start_repl(esp_console_repl_t *) { return ESP_OK; }

extern "C" EMSCRIPTEN_KEEPALIVE int emu_console_exec(const char *line)
{
    if (!line) return -1;
    // The firmware caps a command line at 256 bytes.
    char buf[256];
    if (strlen(line) >= sizeof(buf)) {
        printf("Error: command line too long\n");
        return -1;
    }
    snprintf(buf, sizeof(buf), "%s", line);
    char *argv[16];
    const size_t argc = split_argv(buf, argv, sizeof(argv) / sizeof(argv[0]));
    if (argc == 0) return 0;

    for (const Command &c : commands()) {
        if (c.name != argv[0]) continue;
        const int rc = c.func((int)argc, argv);
        fflush(stdout);
        if (rc != 0) {
            printf("Command returned non-zero error code: 0x%x (%s)\n",
                   (unsigned)rc, esp_err_to_name(rc));
        }
        return rc;
    }
    printf("Unrecognized command\n");
    return -1;
}

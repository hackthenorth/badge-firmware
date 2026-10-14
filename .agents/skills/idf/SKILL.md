---
name: idf
description: Run ESP-IDF commands (idf.py build, reconfigure, flash, monitor, menuconfig, fullclean, size, etc.) for this badge firmware project. The ESP-IDF environment must be activated first via the v5.5.3 activation script — running idf.py without it fails with "command not found" or picks up the wrong toolchain. Use whenever the user asks to build, reconfigure, flash, monitor, run menuconfig, check sizes, or invoke any other idf.py subcommand.
---

# ESP-IDF Commands

This project requires ESP-IDF v5.5.3. The toolchain lives in `~/.espressif/` and must be activated in the current shell before any `idf.py` invocation.

## How to run idf.py

Always prefix the command with the activation script in the same shell invocation. Source the activation script and chain with `&&`:

```bash
. "$HOME/.espressif/tools/activate_idf_v5.5.3.sh" && idf.py <subcommand>
```

The double quotes around the path are intentional — keep them so `$HOME` expands and spaces or special characters can't bite later.

## Common invocations

- Reconfigure (fast, no compile): `. "$HOME/.espressif/tools/activate_idf_v5.5.3.sh" && idf.py reconfigure`
- Build: `. "$HOME/.espressif/tools/activate_idf_v5.5.3.sh" && idf.py build`
- Build a specific component target: `. "$HOME/.espressif/tools/activate_idf_v5.5.3.sh" && idf.py build -t <target>`
- Size summary: `. "$HOME/.espressif/tools/activate_idf_v5.5.3.sh" && idf.py size`
- Per-component sizes: `. "$HOME/.espressif/tools/activate_idf_v5.5.3.sh" && idf.py size-components`
- Full clean: `. "$HOME/.espressif/tools/activate_idf_v5.5.3.sh" && idf.py fullclean`
- Menuconfig: not runnable from Codex — it needs an interactive TTY. Tell the user to run it themselves.
- Flash / monitor: do NOT run these without explicit user permission. They touch the device on a serial port and can interfere with whatever the user has connected.

## Build verification policy

Per project preference: do NOT auto-run full builds after edits. Make changes, summarize what changed, and let the user trigger the build, OR ask first ("want me to run a build?"). `reconfigure` is cheap and acceptable for sanity-checking CMake changes — but still mention you're about to run it.

If the user explicitly says "build it", "verify the build", or pastes a compile error, go ahead and build using the pattern above.

## Working directory

`idf.py` must run from the firmware project root (the top of this repository) — it reads `CMakeLists.txt` and `sdkconfig` there. Use absolute paths in `cd` only if the current working directory has drifted; otherwise just run the command and trust the session cwd.

## Long-running builds

A full `idf.py build` from a clean state can take several minutes. Two options:

- Run with `run_in_background: true` and use Monitor or BashOutput to follow the log without blocking.
- Run in the foreground with an extended `timeout` (e.g. 600000ms) when you specifically need the result before continuing.

Prefer background mode for cold builds; foreground is fine for incremental rebuilds.

## Parsing build output

When a build fails, the actionable lines are usually:
- `error:` lines from the compiler (gcc/clang style — file:line:col)
- `undefined reference to` from the linker
- `CMake Error` from configure-time issues
- Missing-include warnings (`fatal error: foo.h: No such file or directory`)

Pipe through `tail -80` or grep for `error:` / `Error` to keep output bounded. Don't truncate so aggressively that you miss the first error — earlier errors usually cause later ones.

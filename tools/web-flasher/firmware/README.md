# Firmware binaries — factory flasher

These are the **factory-test** binaries (`FACTORY_TEST_BUILD=1`) served by the
factory flasher (the root site with Chinese text). They are **committed to the
repo** so the factory always flashes the exact reviewed image — the
`Deploy Web Flasher` workflow publishes them as-is with no build step.

Three files, with **exactly** these names — the addresses in
[`../index.html`](../index.html) depend on them:

| File                   | Flash offset | Source in `build/`                       |
| ---------------------- | ------------ | ---------------------------------------- |
| `bootloader.bin`       | `0x0`        | `build/bootloader/bootloader.bin`        |
| `partition-table.bin`  | `0x8000`     | `build/partition_table/partition-table.bin` |
| `firmware.bin`         | `0x10000`    | `build/<project-name>.bin`               |

## Refreshing the committed image

Build with the factory-test flag, copy the three files in, then commit them:

```sh
# 1. Build the factory-test image (boots straight into factory_test_run()).
BADGE_FACTORY_TEST=1 idf.py build

# 2. Copy the artifacts in (from the repo root).
APP=$(ls build/*.bin | grep -v -E 'bootloader|partition' | head -1)
cp build/bootloader/bootloader.bin            tools/web-flasher/firmware/bootloader.bin
cp build/partition_table/partition-table.bin  tools/web-flasher/firmware/partition-table.bin
cp "$APP"                                      tools/web-flasher/firmware/firmware.bin

# 3. Commit, then run the Deploy Web Flasher workflow.
git add tools/web-flasher/firmware/*.bin
```

The repo-root `.gitignore` ignores `*.bin`; the `.gitignore` in this folder
re-includes exactly these three names so they can be committed. The engineer
flasher's binaries (`../eng/`) are **not** committed — CI compiles them fresh
(`FACTORY_TEST_BUILD=0`) on each `Deploy Eng Flasher` run.

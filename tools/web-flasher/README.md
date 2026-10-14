# Badge firmware flashing and Lua app uploads

Choose the tool for what you want to install:

| Task | Page | Result |
| --- | --- | --- |
| Write/install a Lua app | [Badge IDE](https://badge.hackthenorth.com/ide/) or [Pages IDE](https://hackathon.github.io/badge-firmware/badgectl/) | Uploads app files to the existing launcher firmware |
| Update normal badge firmware | [Engineer flasher](https://hackathon.github.io/badge-firmware/eng/) | Installs launcher firmware and applies a selected built-in app tier |
| Test factory hardware | [Factory flasher](https://hackathon.github.io/badge-firmware/) | Installs the committed factory-test image |

**Making a custom app? Start with the [complete Lua app guide](../badgectl/web/README.md).**
It includes a prompt you can give a web agent, the supported APIs and limits,
and seven full apps. Each example includes the manifest and all Lua code in a
single `.lua` file. Paste the agent's answer into **Import app** in the IDE,
then **Connect → Push**. You do not need ESP-IDF or a firmware build for that.
The guide is also available through **Download guide** in the IDE.

## Upload a Lua app

1. Use a desktop browser with Web Serial, such as Chrome or Edge, and a USB
   data cable. Close other serial tools/tabs that own the badge's port.
2. Open the IDE, import your single-file app, and check its slug. Import only
   changes the editor; **Push** writes to the badge.
3. Turn the badge off, plug in USB, then turn it on normally. **Do not hold
   Start**: the Lua uploader needs the running firmware's `badge> ` console.
4. Click **Connect**, choose **USB JTAG/serial debug unit** (Espressif), then
   **Push**. Wait for completion and open the app from the badge launcher.
5. Edit and Push again to update that slug. Use a new slug to install another
   app. HOME exits. Reboot after changing runtime manifest settings on an
   already-installed slug; ordinary source edits only need reopening the app.

See the [guide](../badgectl/web/README.md) for manual multi-file editing, custom
icons, persistence, app sharing, upload failures, and complete code examples.

## Flash normal firmware

1. Open the **engineer** page, not the factory page. Both are static browser
   tools using vendored esptool-js and Web Serial; use HTTPS or localhost.
2. Close the IDE/serial monitor's connection and connect the badge with a USB
   data cable. Choose **Apps available after flashing**:

   | Option | Built-in app tier |
   | --- | --- |
   | Default apps only | Default bundle |
   | All public apps | Default + public apps |
   | All apps (public + diagnostics) | Default + public + internal diagnostic apps |

3. Enter the **Admin password** for this firmware. It is not committed, so
   get it from the maintainers. It is used by the console
   step that applies the selected app tier after flashing.
4. Click **Flash a Badge** and select its serial port. Selecting the port starts
   flashing immediately; there is no second confirmation.
5. Wait for the write and app-mode step to finish. If connection fails with
   "No serial data received", unplug, hold **Start** while plugging USB back
   in, then release Start and retry. A blank screen in this download mode is
   expected. If it stays blank after flashing, power-cycle normally with no
   buttons held.
6. If the page says firmware flashed but app mode was not set, the image write
   succeeded. Restart normally and use the [registration tool](https://hackathon.github.io/badge-firmware/register/)
   to apply the intended app mode using the Admin password. You do not need to
   flash the same image again just for that console step.
7. Close/disconnect the flasher before reconnecting in the IDE.

Both flashers write bootloader, partition table and app at `0x0`, `0x8000`, and
`0x10000`, with `eraseAll: false`. They do not upload a LittleFS image or request
an NVS/filesystem erase. This is not a factory reset or a backup. The engineer
page additionally runs the persistent `appmode` command, changing launcher
configuration. Keep firmware and partition layout compatible with the badge.

The root factory page installs hardware-test firmware and offers **Close** when
done for the next badge. Use `/eng/` to restore the normal launcher afterward.

## Serve locally

For **just the Lua IDE**, no binaries or build are needed:

```sh
# From the repository root:
python3 -m http.server 8000 --directory tools/badgectl/web
# Open http://localhost:8000
```

For **firmware flashing**, stage the correct image first. The root
`firmware/` directory already contains committed **factory-test** binaries;
see [firmware/README.md](firmware/README.md) before replacing those.
The engineer page loads `eng/firmware/`, which must contain a matching normal
build's three binaries. To stage an existing normal build from the repo root:

```sh
mkdir -p tools/web-flasher/eng/firmware
cp build/bootloader/bootloader.bin tools/web-flasher/eng/firmware/bootloader.bin
cp build/partition_table/partition-table.bin tools/web-flasher/eng/firmware/partition-table.bin
cp build/hello_world.bin tools/web-flasher/eng/firmware/firmware.bin
python3 -m http.server 8000 --directory tools/web-flasher
# Open http://localhost:8000/eng/
```

If no build exists, follow the repository's [build instructions](../../README.md#build)
first. Do not stage factory-test binaries under `/eng/` and expect a launcher.
Only HTTPS and localhost are supported for Web Serial; opening `index.html`
directly from disk is not the supported setup.

## Deployment and files

[Deploy Web Flasher](../../.github/workflows/deploy-flasher.yml) runs on pushes
to `main` and manual dispatch. It publishes one GitHub Pages artifact:

- `/`: committed factory-test binaries, published as-is.
- `/eng/`: normal launcher firmware built fresh by CI with ESP-IDF v5.5.3.
- `/badgectl/`: copied from `tools/badgectl/web`, including its guide/importer.
- `/register/`: registration/provisioning tool.
- `/blindbox/`: Web Serial scavenger-hunt bridge.
- `/sync-viewer/`: snapshot viewer.

For initial Pages setup, select **Settings → Pages → Source: GitHub Actions**.
Other static hosts work if served over HTTPS with the same directory layout.
The separate badge website build copies the IDE to `/ide/`; edit the canonical
files under `tools/badgectl/web`, not the generated deployment copies.

`index.html` and `eng/index.html` each contain their flashing UI and `PARTS`
offsets. `vendor/esptool-js-0.5.4.bundle.js` is vendored so firmware flashing
does not need a CDN. The IDE uses Monaco from a CDN. The engineer password
field check is `node tools/test-admin-dashboard.mjs` from the repo root.

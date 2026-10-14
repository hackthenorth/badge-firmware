# Vendored browser dependencies

`esptool-js-0.5.4.bundle.js` is the exact bundle the factory web flasher uses
(`tools/web-flasher/vendor/`), copied so `/sync` can flash official firmware
in-browser over Web Serial. If the factory flasher upgrades its bundle, copy
the new file here and update the import in `src/sync/FirmwareFlasher.jsx`.

// Pure helpers for the /sync in-browser firmware flasher
// (FirmwareFlasher.jsx). Web Serial and esptool-js stay in the component so
// node tests can prove the flash plan and progress math without hardware.

export const FIRMWARE_MANIFEST_URL = "/firmware/firmware.json";
export const ESPTOOL_BUNDLE_URL =
  "/vendor/esptool-js-0.5.4.bundle.js";

// Turns the committed firmware manifest into an ordered esptool flash plan:
// each file's site URL plus its numeric flash address, sorted by address.
export function flashPlan(manifest) {
  const files = manifest?.files;
  if (!Array.isArray(files) || files.length === 0) {
    throw new Error("The official firmware manifest is missing its file list.");
  }
  return [...files]
    .map((file) => {
      const address = Number.parseInt(file?.offset, 16);
      if (typeof file?.name !== "string" || !Number.isSafeInteger(address)) {
        throw new Error("The official firmware manifest is malformed.");
      }
      return { name: file.name, url: `/firmware/${file.name}`, address };
    })
    .sort((a, b) => a.address - b.address);
}

// Overall flash percentage across all parts, matching the factory flasher:
// completed parts plus the in-progress part's written bytes.
export function overallProgress(sizes, index, written) {
  const total = sizes.reduce((sum, size) => sum + size, 0);
  if (total <= 0) return 0;
  const done =
    sizes.slice(0, index).reduce((sum, size) => sum + size, 0) + written;
  return Math.min(100, Math.round((done / total) * 100));
}

export const CONNECT_TIMEOUT_MS = 45000;

// Rejects when the badge bootloader never answers, so the UI can say how to
// recover (download mode, close other serial tabs) instead of spinning.
// The underlying attempt keeps running; the caller must release the port.
export function withTimeout(promise, ms, onTimeout) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(onTimeout()), ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

export function connectTimeoutError() {
  return new Error(
    "Couldn't reach the badge bootloader. Unplug the badge, hold Start " +
      "while plugging it back in (a blank screen means download mode, not " +
      "a brick), close the Badge IDE or any other serial tab, then flash again."
  );
}

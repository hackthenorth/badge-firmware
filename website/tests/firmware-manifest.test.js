import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const testsDirectory = dirname(fileURLToPath(import.meta.url));
const firmwareDirectory = resolve(testsDirectory, "../public/firmware");

const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

test("the committed firmware manifest matches the downloadable binaries", async () => {
  const manifest = JSON.parse(
    await readFile(resolve(firmwareDirectory, "firmware.json"), "utf8")
  );
  assert.equal(manifest.label, "official badge firmware");
  assert.match(manifest.identity, /^[0-9a-f]{64}$/);

  // Same digest rapid-flash prints: each part's little-endian load address
  // followed by its raw bytes, in address order.
  const identity = createHash("sha256");
  let totalBytes = 0;
  const parts = [...manifest.files].sort(
    (a, b) => parseInt(a.offset, 16) - parseInt(b.offset, 16)
  );
  for (const file of parts) {
    const bytes = await readFile(resolve(firmwareDirectory, file.name));
    assert.equal(bytes.length, file.size, `${file.name} size`);
    assert.equal(sha256(bytes), file.sha256, `${file.name} sha256`);
    const address = Buffer.alloc(4);
    address.writeUInt32LE(parseInt(file.offset, 16));
    identity.update(address);
    identity.update(bytes);
    totalBytes += bytes.length;
  }
  assert.equal(identity.digest("hex"), manifest.identity);
  assert.equal(totalBytes, manifest.totalBytes);
  assert.deepEqual(
    parts.map((file) => [file.name, file.offset]),
    [
      ["bootloader.bin", "0x0"],
      ["partition-table.bin", "0x8000"],
      ["firmware.bin", "0x10000"],
    ]
  );
});

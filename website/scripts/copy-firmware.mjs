// Copies the official badge image set baked into tools/rapid-flash into the
// website's public firmware directory and writes firmware.json.
//
// The three binaries are committed (see firmware/.gitignore) so the live
// site serves the exact reviewed image, mirroring tools/web-flasher. Refresh
// after rebuilding rapid-flash's badge set:
//
//   cargo run --release --manifest-path tools/rapid-flash/Cargo.toml -- --once
//   node website/scripts/copy-firmware.mjs   # from the repo root
//   git add website/public/firmware
//
// The manifest's `identity` is the same digest rapid-flash prints on startup
// (SHA-256 over each part's 4-byte little-endian load address followed by
// its raw bytes, in address order), so the hash on /sync matches the mosaic
// source operators compare across flash stations.

import { createHash } from "node:crypto";
import { copyFile, mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(scriptDirectory, "..", "..");
const sourceDirectory = resolve(repoRoot, "tools/rapid-flash/firmware");
const destinationDirectory = resolve(repoRoot, "website/public/firmware");

const PARTS = [
  { offset: 0x0, source: "bootloader/bootloader.bin", name: "bootloader.bin" },
  {
    offset: 0x8000,
    source: "partition_table/partition-table.bin",
    name: "partition-table.bin",
  },
  { offset: 0x10000, source: "hello_world.bin", name: "firmware.bin" },
];

const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

const head = process.argv.includes("--check")
  ? "check"
  : "copy";

const manifestPath = resolve(destinationDirectory, "firmware.json");
const files = [];
const identity = createHash("sha256");
for (const part of [...PARTS].sort((a, b) => a.offset - b.offset)) {
  const bytes = await readFile(resolve(sourceDirectory, part.source));
  const digest = sha256(bytes);
  files.push({
    name: part.name,
    offset: `0x${part.offset.toString(16)}`,
    size: bytes.length,
    sha256: digest,
  });
  const address = Buffer.alloc(4);
  address.writeUInt32LE(part.offset);
  identity.update(address);
  identity.update(bytes);
}

const next = {
  label: "official badge firmware",
  identity: identity.digest("hex"),
  totalBytes: files.reduce((sum, file) => sum + file.size, 0),
  files,
};

if (head === "check") {
  const manifest = JSON.parse(await readFile(manifestPath, "utf8"));
  const subset = Object.fromEntries(
    Object.entries(manifest).filter(([key]) => key in next)
  );
  if (JSON.stringify(subset) !== JSON.stringify(next)) {
    console.error(
      "website/public/firmware/firmware.json is stale: run node website/scripts/copy-firmware.mjs"
    );
    process.exit(1);
  }
  console.log("firmware manifest is current");
} else {
  await mkdir(destinationDirectory, { recursive: true });
  for (const part of PARTS) {
    await copyFile(
      resolve(sourceDirectory, part.source),
      resolve(destinationDirectory, part.name)
    );
  }
  await writeFile(
    manifestPath,
    `${JSON.stringify(
      {
        ...next,
        source: "tools/rapid-flash/firmware",
        refreshed: new Date().toISOString().slice(0, 10),
      },
      null,
      2
    )}\n`
  );
  console.log(
    `copied ${files.length} firmware files (${next.totalBytes} bytes), identity ${next.identity.slice(0, 12)}…`
  );
}

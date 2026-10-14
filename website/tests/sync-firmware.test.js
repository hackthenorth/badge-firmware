import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import {
  connectTimeoutError,
  flashPlan,
  overallProgress,
  withTimeout,
} from "../src/sync/firmware.js";

const testsDirectory = dirname(fileURLToPath(import.meta.url));
const manifest = JSON.parse(
  await readFile(resolve(testsDirectory, "../public/firmware/firmware.json"), "utf8")
);

test("the flash plan covers the image set in address order", () => {
  assert.deepEqual(flashPlan(manifest), [
    { name: "bootloader.bin", url: "/firmware/bootloader.bin", address: 0x0 },
    {
      name: "partition-table.bin",
      url: "/firmware/partition-table.bin",
      address: 0x8000,
    },
    { name: "firmware.bin", url: "/firmware/firmware.bin", address: 0x10000 },
  ]);
});

test("a malformed manifest fails before touching USB", () => {
  for (const bad of [
    null,
    {},
    { files: [] },
    { files: [{ name: "firmware.bin" }] },
    { files: [{ name: "firmware.bin", offset: "nope" }] },
    { files: [{ offset: "0x10000" }] },
  ]) {
    assert.throws(() => flashPlan(bad), /manifest/);
  }
});

test("a hung connect rejects with download-mode recovery steps", async () => {
  const hanging = new Promise(() => {});
  await assert.rejects(
    withTimeout(hanging, 5, connectTimeoutError),
    /hold Start while plugging it back in/
  );
  assert.match(connectTimeoutError().message, /Couldn't reach the badge/);
});

test("a connect that answers in time is left alone", async () => {
  assert.equal(await withTimeout(Promise.resolve("bootloader"), 50, connectTimeoutError), "bootloader");
  await assert.rejects(
    withTimeout(Promise.reject(new Error("no serial data")), 50, connectTimeoutError),
    /no serial data/
  );
});

test("progress spans all parts and clamps at 100", () => {
  const sizes = [21024, 3072, 2651840];
  assert.equal(overallProgress(sizes, 0, 0), 0);
  assert.equal(overallProgress(sizes, 0, 10512), 0);
  assert.equal(overallProgress(sizes, 2, 1325920), 50);
  assert.equal(overallProgress(sizes, 2, 2651840), 100);
  assert.equal(overallProgress(sizes, 2, 9999999), 100);
  assert.equal(overallProgress([], 0, 0), 0);
});

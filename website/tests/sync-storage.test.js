import test from "node:test";
import assert from "node:assert/strict";
import {
  LAST_SNAPSHOT_KEY,
  loadLastSnapshot,
  saveLastSnapshot,
  saveIdentityBackup,
  IDENTITY_BACKUP_KEY,
} from "../src/sync/storage.js";
import { demoSnapshot } from "../src/sync/demo.js";

const record = (id = "first") => ({
  snapshot: { ...structuredClone(demoSnapshot), badge_id: id },
  beforeUnlock: structuredClone(demoSnapshot),
  beforeIdentity: structuredClone(demoSnapshot),
  source: "Haker Badge",
  readAt: new Date("2026-09-19T20:00:00Z"),
});
function memoryStorage() {
  const values = new Map();
  return {
    getItem: (key) => values.get(key) ?? null,
    setItem: (key, value) => values.set(key, value),
  };
}

test("restores the last complete record, including its original read time and before-unlock backup", () => {
  const storage = memoryStorage();
  assert.equal(loadLastSnapshot(storage), null);
  const first = record();
  assert.equal(saveLastSnapshot(first, storage), true);
  assert.deepEqual(loadLastSnapshot(storage), first);
  const next = record("second");
  next.beforeUnlock = null;
  assert.equal(saveLastSnapshot(next, storage), true);
  assert.deepEqual(loadLastSnapshot(storage), next);
});

test("connecting or a failed read does not erase the last successful snapshot", () => {
  const storage = memoryStorage();
  const saved = record();
  saveLastSnapshot(saved, storage);
  saveLastSnapshot(
    { snapshot: null, beforeUnlock: null, source: "", readAt: null },
    storage
  );
  assert.deepEqual(loadLastSnapshot(storage), saved);
});

test("corrupt, outdated, oversized, and malformed stored data do not break page load", () => {
  const storage = memoryStorage();
  const saved = { ...record(), version: 1 };
  for (const raw of [
    "broken json",
    "null",
    "x".repeat(200000),
    JSON.stringify({ ...saved, version: 2 }),
    JSON.stringify({ ...saved, snapshot: {} }),
    JSON.stringify({ ...saved, beforeUnlock: {} }),
    JSON.stringify({ ...saved, beforeIdentity: {} }),
    JSON.stringify({ ...saved, source: {} }),
    JSON.stringify({ ...saved, readAt: "invalid date" }),
  ]) {
    storage.setItem(LAST_SNAPSHOT_KEY, raw);
    assert.equal(loadLastSnapshot(storage), null);
  }
});

test("unavailable storage and quota failures leave sync and downloads usable", () => {
  const blocked = {
    getItem() {
      throw new Error("Access denied");
    },
    setItem() {
      throw new Error("Quota exceeded");
    },
  };
  assert.equal(loadLastSnapshot(blocked), null);
  assert.equal(saveLastSnapshot(record(), blocked), false);
  assert.equal(saveIdentityBackup({ raw: null, badgeId: "", provisioned: false }, blocked), false);
});

test("identity recovery is saved independently without replacing the original event snapshot", () => {
  const storage = memoryStorage();
  const snapshot = record();
  saveLastSnapshot(snapshot, storage);
  const identity = { raw: '{"badge_id":"acorn-river-moon-fox"}', badgeId: "acorn-river-moon-fox", provisioned: true };
  assert.equal(saveIdentityBackup(identity, storage), true);
  const backup = JSON.parse(storage.getItem(IDENTITY_BACKUP_KEY));
  assert.equal(backup.raw, identity.raw);
  assert.equal(backup.badgeId, identity.badgeId);
  assert.ok(Number.isFinite(Date.parse(backup.savedAt)));
  assert.deepEqual(loadLastSnapshot(storage), snapshot);
});

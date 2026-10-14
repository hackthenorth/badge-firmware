import { MAX_SNAPSHOT_BYTES, parseSnapshot } from "./data.js";

export const IDENTITY_BACKUP_KEY = "htn-badge:identity-backup:v1";

export function saveIdentityBackup(record, storage) {
  try {
    (storage ?? globalThis.localStorage).setItem(
      IDENTITY_BACKUP_KEY,
      JSON.stringify({
        version: 1,
        savedAt: new Date().toISOString(),
        raw: record.raw,
        badgeId: record.badgeId,
        provisioned: record.provisioned,
      })
    );
    return true;
  } catch {
    return false;
  }
}

export const LAST_SNAPSHOT_KEY = "htn-badge:last-snapshot:v1";

export function loadLastSnapshot(storage) {
  try {
    const raw = (storage ?? globalThis.localStorage).getItem(LAST_SNAPSHOT_KEY);
    if (!raw || raw.length > MAX_SNAPSHOT_BYTES * 3 + 1024) return null;
    const saved = JSON.parse(raw);
    if (
      saved.version !== 1 ||
      typeof saved.source !== "string" ||
      saved.source.length > 100 ||
      typeof saved.readAt !== "string" ||
      !Number.isFinite(Date.parse(saved.readAt))
    )
      return null;
    return {
      snapshot: parseSnapshot(JSON.stringify(saved.snapshot)),
      beforeUnlock: saved.beforeUnlock
        ? parseSnapshot(JSON.stringify(saved.beforeUnlock))
        : null,
      beforeIdentity: saved.beforeIdentity
        ? parseSnapshot(JSON.stringify(saved.beforeIdentity))
        : null,
      source: saved.source,
      readAt: new Date(saved.readAt),
    };
  } catch {
    // Corrupt or unavailable storage must never prevent connecting a badge.
    return null;
  }
}

export function saveLastSnapshot(
  { snapshot, beforeUnlock, beforeIdentity, source, readAt },
  storage
) {
  // Connecting clears the displayed badge, but keep the last successful read
  // until another snapshot replaces it. A cancelled/failed sync cannot erase it.
  if (!snapshot) return true;
  try {
    (storage ?? globalThis.localStorage).setItem(
      LAST_SNAPSHOT_KEY,
      JSON.stringify({
        version: 1,
        snapshot,
        beforeUnlock,
        beforeIdentity,
        source,
        readAt: readAt.toISOString(),
      })
    );
    return true;
  } catch {
    return false;
  }
}

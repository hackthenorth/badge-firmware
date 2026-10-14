import { decodeBadgeSummary, PUBLIC_APPS } from "./summary.js";
import { decodeBadgeContacts } from "./contacts.js";

export { PUBLIC_APPS };
export const MAX_SNAPSHOT_BYTES = 65536;
const encoder = new TextEncoder();
export const normalizePath = (path) => path.replace(/^\/littlefs\//, "");

export function parseSnapshot(raw) {
  if (encoder.encode(raw).length > MAX_SNAPSHOT_BYTES)
    throw new Error("This snapshot is too large (maximum 64 KB).");
  let doc;
  try {
    doc = JSON.parse(raw);
  } catch {
    throw new Error("The badge snapshot is not valid JSON. Try syncing again.");
  }
  if (
    !doc ||
    doc.v !== 1 ||
    doc.kind !== "fs_mirror" ||
    !doc.files ||
    typeof doc.files !== "object" ||
    Array.isArray(doc.files)
  ) {
    throw new Error("Choose a badge filesystem snapshot (v1 fs_mirror).");
  }
  for (const entry of Object.values(doc.files)) {
    if (
      !entry ||
      typeof entry !== "object" ||
      Array.isArray(entry) ||
      (entry.text !== undefined && typeof entry.text !== "string") ||
      (entry.skip !== undefined && typeof entry.skip !== "string") ||
      (entry.trunc !== undefined && typeof entry.trunc !== "boolean") ||
      (entry.size !== undefined &&
        (!Number.isSafeInteger(entry.size) || entry.size < 0))
    ) {
      throw new Error(
        "The snapshot contains an invalid file entry. Try syncing again."
      );
    }
  }
  return doc;
}

export function textFile(doc, path) {
  const matches = Object.entries(doc.files).filter(
    ([name]) => normalizePath(name) === path
  );
  if (matches.length !== 1) return null;
  const file = matches[0][1];
  return file && typeof file.text === "string" && !file.trunc && !file.skip
    ? file.text
    : null;
}

export function fields(text) {
  return Object.fromEntries(
    (text || "").split(/\r?\n/).flatMap((line) => {
      const i = line.indexOf("=");
      return i > 0 ? [[line.slice(0, i), line.slice(i + 1)]] : [];
    })
  );
}

export function describeSnapshot(doc) {
  const raw = JSON.stringify(doc);
  const summary = decodeBadgeSummary(raw);
  const connections = decodeBadgeContacts(raw);
  let identity = {};
  try {
    identity = JSON.parse(textFile(doc, "identity.json") || "{}");
  } catch {
    /* raw file stays available */
  }
  const files = Object.entries(doc.files)
    .map(([path, entry]) => ({ path, entry }))
    .sort((a, b) => a.path.localeCompare(b.path));
  const visits = files.flatMap(({ path }) => {
    const normalized = normalizePath(path);
    if (!/^config\/(sponsors|workshops)\/[^/]+\.cfg$/.test(normalized))
      return [];
    const text = textFile(doc, normalized);
    return text === null
      ? []
      : [
          {
            ...fields(text),
            kind: normalized.includes("/sponsors/") ? "Sponsor" : "Workshop",
            path,
          },
        ];
  });
  const issues = files.filter(
    ({ entry }) =>
      !entry || entry.trunc || entry.skip || typeof entry.text !== "string"
  );
  const partial =
    doc.complete !== true ||
    issues.some(({ entry }) => entry?.skip !== "binary") ||
    summary?.partial ||
    connections.partial;
  return {
    summary,
    contacts: connections.contacts,
    identity,
    files,
    visits,
    issues,
    partial,
  };
}

export function csv(rows) {
  const cell = (value) => {
    let text = String(value ?? "");
    // Spreadsheet apps interpret leading formula characters, even after whitespace.
    if (/^[\s\uFEFF]*[=+@-]|^[\t\r\n]/.test(text)) text = "'" + text;
    return '"' + text.replaceAll('"', '""') + '"';
  };
  return (
    "\uFEFF" + rows.map((row) => row.map(cell).join(",")).join("\r\n") + "\r\n"
  );
}

export function contactsCsv(contacts) {
  const labels = [
    "Email",
    "Phone",
    "LinkedIn",
    "Discord",
    "Instagram",
    "X / Twitter",
  ];
  return csv([
    [
      "Name",
      "Role",
      ...labels.flatMap((label) =>
        label === "Email" || label === "Phone"
          ? [label]
          : [label, `${label} link`]
      ),
      "Badge ID",
    ],
    ...contacts.map((contact) => [
      contact.name,
      contact.role,
      ...labels.flatMap((label) => {
        const detail = contact.details.find((item) => item.label === label);
        return label === "Email" || label === "Phone"
          ? [detail?.value]
          : [detail?.value, detail?.href];
      }),
      contact.badgeId,
    ]),
  ]);
}

export function recordsCsv(summary) {
  return csv([
    ["Game", "Record", "Value", "Unit"],
    ...(summary?.games || []).flatMap((game) =>
      game.metrics.map((metric) => [
        game.name,
        metric.label,
        metric.value,
        metric.unit,
      ])
    ),
  ]);
}

export function progressCsv(summary) {
  return csv([
    ["Progress", "Collected", "Total"],
    ...(summary?.progress || []).map((row) => [
      row.label,
      row.count,
      row.total,
    ]),
  ]);
}

export function visitsCsv(visits) {
  const keys = [...new Set(visits.flatMap(Object.keys))];
  return csv([keys, ...visits.map((visit) => keys.map((key) => visit[key]))]);
}

// Existing firmware's public USB `put` command needs no token. Keep every other
// config line and existing app order; never grant internal/diagnostic apps.
export function planUnlock(doc) {
  const original = textFile(doc, "config/system.cfg");
  if (original === null)
    throw new Error(
      "The launcher settings are missing or incomplete. Sync again before unlocking."
    );
  const lines = original.replace(/\r\n/g, "\n").split("\n");
  const orders = lines.filter((line) => line.startsWith("launcher_order="));
  if (orders.length !== 1)
    throw new Error(
      "The badge has ambiguous launcher settings. Unlock cancelled; your data is unchanged."
    );
  const existing = orders[0]
    .slice("launcher_order=".length)
    .split(",")
    .filter(Boolean);
  if (existing.some((slug) => !/^[a-z0-9_-]+$/i.test(slug)))
    throw new Error("The launcher contains an unsupported app name.");
  const added = PUBLIC_APPS.filter((slug) => !existing.includes(slug));
  const order = [...existing, ...added].join(",");
  // Firmware reads into a 1024-byte buffer, including the final NUL.
  if (encoder.encode(order).length >= 1024)
    throw new Error(
      "The launcher is full. Remove some custom apps before unlocking."
    );
  const updated = lines
    .map((line) =>
      line.startsWith("launcher_order=") ? `launcher_order=${order}` : line
    )
    .join("\n");
  if (encoder.encode(updated).length > 4096)
    throw new Error(
      "Launcher settings are too large to verify safely. Unlock cancelled."
    );
  return { original, updated, added };
}

export function download(name, text, type = "application/json") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const link = document.createElement("a");
  link.href = url;
  link.download = name.replace(/[^a-z0-9_.-]/gi, "_");
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

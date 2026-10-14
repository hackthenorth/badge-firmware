// Adapted from my.hackthenorth.com/src/views/badge/connect/contacts.ts.
function decodeBadgeContacts(raw) {
  if (!raw) return { contacts: [], partial: false };
  try {
    if (raw.length > 65536) throw new Error();
    const snapshot = JSON.parse(raw);
    if (
      snapshot?.v !== 1 ||
      snapshot.kind !== "fs_mirror" ||
      !snapshot.files ||
      typeof snapshot.files !== "object" ||
      Array.isArray(snapshot.files)
    )
      throw new Error();
    const contacts = /* @__PURE__ */ new Map();
    let partial = snapshot.complete === false;
    for (const [path, entry] of Object.entries(snapshot.files)) {
      if (!/^(?:\/littlefs\/)?config\/contacts\/[a-z0-9_-]+\.cfg$/i.test(path))
        continue;
      const file = entry;
      if (
        !file ||
        typeof file.text !== "string" ||
        file.trunc ||
        file.skip ||
        file.text.length > 2048
      ) {
        partial = true;
        continue;
      }
      const fields = /* @__PURE__ */ Object.create(null);
      for (const line of file.text.split(/\r?\n/)) {
        const separator = line.indexOf("=");
        if (separator > 0)
          fields[line.slice(0, separator)] = line.slice(separator + 1).trim();
      }
      if (!/^[a-z0-9_-]{1,63}$/i.test(fields.badge_id || "")) {
        partial = true;
        continue;
      }
      const contact = {
        badgeId: fields.badge_id,
        name: fields.name?.slice(0, 100) || "Unnamed contact",
        role:
          [
            "Hacker",
            "Organizer",
            "Sponsor",
            "Judge",
            "Mentor",
            "Volunteer",
            "Media",
            "Staff",
            "General",
            "Workshop lead",
            "Visitor",
          ][Number(fields.role)] || "Unknown role",
        receivedUnix:
          /^\d+$/.test(fields.received_unix || "") &&
          Number.isSafeInteger(Number(fields.received_unix))
            ? Number(fields.received_unix)
            : null,
        details: [],
      };
      for (const [key, label] of [
        ["email", "Email"],
        ["phone", "Phone"],
        ["linkedin", "LinkedIn"],
        ["discord", "Discord"],
        ["instagram", "Instagram"],
        ["x", "X / Twitter"],
      ]) {
        let value = fields[key];
        if (!value) continue;
        if (key === "email" || key === "phone") {
          value = value.replace(key === "email" ? /^mailto:/i : /^tel:/i, "");
          try {
            value = decodeURIComponent(value);
          } catch {
            // Keep malformed escapes readable without breaking other contacts.
          }
        }
        let href;
        if (
          key === "email" && // Control characters must never become mail headers or URL content.
          // eslint-disable-next-line no-control-regex
          /^[^\s@\x00-\x1f]+@[^\s@\x00-\x1f]+\.[^\s@\x00-\x1f]+$/.test(value)
        )
          href = `mailto:${encodeURIComponent(value)}`;
        else if (key === "phone" && /^\+?[0-9 ()-]{3,30}$/.test(value))
          href = `tel:${value.replace(/[ ()-]/g, "")}`;
        else if (["linkedin", "instagram", "x"].includes(key)) {
          let handle = value.replace(/^@/, "");
          if (value.includes("/")) {
            try {
              const url = new URL(
                /^https?:\/\//i.test(value) ? value : `https://${value}`
              );
              const hosts =
                key === "linkedin"
                  ? ["linkedin.com", "www.linkedin.com"]
                  : key === "instagram"
                  ? ["instagram.com", "www.instagram.com"]
                  : ["x.com", "www.x.com", "twitter.com", "www.twitter.com"];
              handle =
                hosts.includes(url.hostname) &&
                !url.username &&
                !url.password &&
                ["https:", "http:"].includes(url.protocol)
                  ? url.pathname.split("/").filter(Boolean).pop() || ""
                  : "";
            } catch {
              handle = "";
            }
          }
          if (/^[\p{L}\p{N}_.-]+$/u.test(handle)) {
            href = `${
              key === "linkedin"
                ? "https://www.linkedin.com/in/"
                : key === "instagram"
                ? "https://www.instagram.com/"
                : "https://x.com/"
            }${encodeURIComponent(handle)}`;
          }
        }
        contact.details.push({ label, value, ...(href ? { href } : {}) });
      }
      const previous = contacts.get(contact.badgeId);
      if (
        !previous ||
        (contact.receivedUnix ?? 0) >= (previous.receivedUnix ?? 0)
      )
        contacts.set(contact.badgeId, contact);
    }
    return {
      contacts: Array.from(contacts.values()).sort(
        (a, b) =>
          a.name.localeCompare(b.name, void 0, {
            sensitivity: "base",
            numeric: true,
          }) || a.badgeId.localeCompare(b.badgeId)
      ),
      partial,
    };
  } catch {
    return {
      contacts: [],
      partial: false,
      error:
        "We couldn\u2019t read your saved contact list. Sync your badge again to update it.",
    };
  }
}
export { decodeBadgeContacts };

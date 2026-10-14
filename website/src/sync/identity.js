import { normalizePath, textFile } from "./data.js";

// Firmware contract: main/hal/hal_identity.h (capacities exclude the final NUL).
export const IDENTITY_FIELDS = [
  { key: "display_name", label: "Display name", max: 39, required: true },
  { key: "badge_id", label: "Badge ID", max: 63, required: true },
  { key: "account_email", label: "Account email", max: 63 },
  { key: "net_email", label: "Connection email", max: 63 },
  { key: "net_phone", label: "Phone", max: 19 },
  { key: "net_linkedin", label: "LinkedIn handle", max: 39 },
  { key: "net_discord", label: "Discord", max: 39 },
  { key: "net_instagram", label: "Instagram handle", max: 31 },
  { key: "net_x", label: "X / Twitter handle", max: 31 },
];
export const IDENTITY_ROLES = [
  ["hacker", "Hacker"],
  ["organizer", "Organizer"],
  ["sponsor", "Sponsor"],
  ["judge", "Judge"],
  ["mentor", "Mentor"],
  ["volunteer", "Volunteer"],
  ["media", "Media"],
  ["staff", "Staff"],
  ["general", "General"],
  ["workshop_lead", "Workshop lead"],
  ["visitor", "Visitor"],
];

export const FIELD_HINTS = {
  badge_id:
    "Four short words, such as acorn-river-moon-fox. Keep your current ID to keep the same identity.",
  account_email:
    "The email associated with the account on your badge. It isn’t shared in badge connections. Changing it doesn’t create or sign in to an online account.",
  net_email:
    "An email to share with people you connect with, such as alex@example.com.",
  net_phone: "Include your country code, such as +1 519 555 0123.",
  net_linkedin:
    "Your profile handle or LinkedIn profile URL, such as alex-hacker.",
  net_discord:
    "Your Discord username, such as alex.hacker (or a legacy name#1234).",
  net_instagram: "Your Instagram username or profile URL, such as alex.hacker.",
  net_x: "Your X username or profile URL, such as alex_hacker.",
};

// These intentionally accept common contact formats rather than trying to
// prove an address/account exists. Optional contact fields may be left blank.
const EMAIL = /^[^\s@]+@[^\s@.]+(?:\.[^\s@.]+)+$/;
const CONTACT_RULES = {
  account_email: [EMAIL, "Enter an email address, such as alex@example.com."],
  net_email: [EMAIL, "Enter an email address, such as alex@example.com."],
  net_phone: [
    /^\+?[0-9 ()-]+$/,
    "Use a phone number with 7–15 digits, optionally starting with +.",
  ],
  net_linkedin: [
    /^[a-zA-Z0-9][a-zA-Z0-9-]*$/,
    "Enter a LinkedIn profile handle or profile URL.",
  ],
  net_discord: [
    /^(?:[a-z0-9_.]{2,32}|[^\s#]{2,32}#[0-9]{4})$/,
    "Enter a Discord username using letters, numbers, periods, or underscores, or a legacy name#1234.",
  ],
  net_instagram: [
    /^[a-zA-Z0-9_](?:[a-zA-Z0-9_.]{0,28}[a-zA-Z0-9_])?$/,
    "Use an Instagram handle with letters, numbers, periods, or underscores.",
  ],
  net_x: [
    /^[a-zA-Z0-9_]{1,15}$/,
    "Use an X handle with up to 15 letters, numbers, or underscores.",
  ],
};

export function normalizeIdentityField(key, value) {
  let text = typeof value === "string" ? value.trim() : value;
  const sites = {
    net_linkedin: ["linkedin.com", /^\/in\/([^/]+)\/?$/],
    net_instagram: ["instagram.com", /^\/([^/]+)\/?$/],
    net_x: ["x.com", /^\/([^/]+)\/?$/],
  };
  if (sites[key] && typeof text === "string") {
    if (
      /^(?:https?:\/\/|(?:www\.)?(?:linkedin|instagram|twitter|x)\.com\/)/i.test(
        text
      )
    ) {
      try {
        const url = new URL(
          /^https?:\/\//i.test(text) ? text : `https://${text}`
        );
        const [host, path] = sites[key];
        const actualHost = url.hostname.replace(/^www\./, "");
        const match = url.pathname.match(path);
        if (
          match &&
          (actualHost === host ||
            (key === "net_x" && actualHost === "twitter.com"))
        )
          text = decodeURIComponent(match[1]);
      } catch {
        /* validation will explain the expected format */
      }
    }
    text = text.replace(/^@/, "");
  }
  if (key === "net_discord" && typeof text === "string")
    text = text.replace(/^@/, "");
  return text;
}

export function identityFieldError(key, input, base = {}) {
  const field = IDENTITY_FIELDS.find((field) => field.key === key);
  if (!field) return "";
  const value = normalizeIdentityField(key, input);
  if (typeof value !== "string") return `${field.label} must be text.`;
  if (!value) return field.required ? `${field.label} is required.` : "";
  if (new TextEncoder().encode(value).length > field.max)
    return `${field.label} is too long for the badge (maximum ${field.max} bytes).`;
  if (!/^[\x20-\x7e\xa0-\xff]*$/.test(value))
    return `${field.label} contains characters the badge cannot display. Use Latin letters, numbers, and punctuation.`;
  if (key === "badge_id") {
    if (value === base.badge_id && /^[a-z0-9_-]+$/i.test(value)) return "";
    if (!/^[a-z]+(?:-[a-z]+){3}$/.test(value))
      return "Use four lowercase words separated by dashes, or click Generate.";
  }
  const rule = CONTACT_RULES[key];
  if (rule && !rule[0].test(value)) return rule[1];
  if (key === "net_phone" && !/^\d{7,15}$/.test(value.replace(/\D/g, "")))
    return CONTACT_RULES.net_phone[1];
  if (
    ((key === "net_discord" && !value.includes("#")) ||
      key === "net_instagram") &&
    value.includes("..")
  )
    return "The username cannot contain two periods in a row.";
  return "";
}

export function readIdentity(snapshot) {
  if (!snapshot) return {};
  const entries = Object.keys(snapshot.files).filter(
    (path) => normalizePath(path) === "identity.json"
  );
  if (
    !entries.length &&
    snapshot.provisioned === false &&
    snapshot.complete === true
  )
    return {};
  const text = textFile(snapshot, "identity.json");
  if (text === null)
    throw new Error(
      "The identity is missing or incomplete. Sync again before editing it."
    );
  let identity;
  try {
    identity = JSON.parse(text);
  } catch {
    throw new Error(
      "The saved identity is not valid JSON. Keep your snapshot for recovery."
    );
  }
  if (!identity || typeof identity !== "object" || Array.isArray(identity))
    throw new Error("The saved identity is invalid.");
  return identity;
}

export function identityDraft(base = {}) {
  const role =
    typeof base.role === "number"
      ? IDENTITY_ROLES[base.role]?.[0]
      : IDENTITY_ROLES.find(
          ([key, label]) =>
            key === String(base.role).toLowerCase() ||
            label.toLowerCase() === String(base.role).toLowerCase()
        )?.[0];
  let color = base.role_color;
  if (
    Array.isArray(color) &&
    color.length === 3 &&
    color.every((n) => Number.isInteger(n) && n >= 0 && n <= 255)
  ) {
    color = "#" + color.map((n) => n.toString(16).padStart(2, "0")).join("");
  }
  if (typeof color === "string" && /^[0-9a-f]{6}$/i.test(color))
    color = "#" + color;
  return {
    ...Object.fromEntries(
      IDENTITY_FIELDS.map(({ key }) => [
        key,
        typeof base[key] === "string" ? base[key] : "",
      ])
    ),
    role: role || "hacker",
    role_color:
      typeof color === "string" && /^#[0-9a-f]{6}$/i.test(color) ? color : "",
  };
}

export function buildIdentity(base, draft, now = Date.now()) {
  const identity = { ...base, version: 1 };
  const encoder = new TextEncoder();
  for (const { key, label, max, required } of IDENTITY_FIELDS) {
    const error = identityFieldError(key, draft[key], base);
    if (error) throw new Error(error);
    identity[key] = normalizeIdentityField(key, draft[key]);
  }
  if (!IDENTITY_ROLES.some(([key]) => draft.role === key))
    throw new Error("Choose a supported badge role.");
  identity.role = draft.role;
  if (draft.role_color) {
    if (!/^#[0-9a-f]{6}$/i.test(draft.role_color))
      throw new Error("Choose a valid role color.");
    identity.role_color = draft.role_color;
  } else delete identity.role_color; // firmware chooses the selected role's default
  // Event registration metadata and other fields are preserved, not fabricated.
  identity.attendee_id ??= 0;
  identity.provisioned_unix ??= Math.floor(now / 1000);
  for (const key of ["attendee_id", "provisioned_unix"]) {
    const value = Number(identity[key]);
    if (!Number.isInteger(value) || value < 0 || value > 0xffffffff)
      throw new Error(`The saved ${key} is invalid.`);
  }
  if (
    identity.claim_id != null &&
    (typeof identity.claim_id !== "string" ||
      encoder.encode(identity.claim_id).length > 23 ||
      /[\x00-\x1f]/.test(identity.claim_id))
  )
    throw new Error("The saved claim ID is invalid.");
  if (encoder.encode(JSON.stringify(identity)).length > 4096)
    throw new Error("This identity is too large for the badge.");
  return identity;
}

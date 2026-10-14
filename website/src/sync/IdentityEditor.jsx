import React, { useState } from "react";
import {
  buildIdentity,
  IDENTITY_FIELDS,
  IDENTITY_ROLES,
  FIELD_HINTS,
  identityDraft,
  identityFieldError,
  normalizeIdentityField,
} from "./identity.js";
import { generateBadgeId } from "./badge-id.js";

export default function IdentityEditor({
  record,
  connected,
  busy,
  enabled,
  onSave,
  onReload,
  loadError,
  status,
}) {
  const base = record?.identity || {};
  const [draft, setDraft] = useState(() => identityDraft(base));
  const [touched, setTouched] = useState({});
  const [error, setError] = useState("");
  const locked = busy || !connected || !record || Boolean(loadError);
  const change = (key, value) => {
    setDraft((old) => ({ ...old, [key]: value }));
    setError("");
  };
  function field({ key, label, max, required }) {
    const issue = touched[key] ? identityFieldError(key, draft[key], base) : "";
    const normalized = normalizeIdentityField(key, draft[key]);
    const hintId = `identity-${key}-hint`;
    const errorId = `identity-${key}-error`;
    return (
      <div className="sync-identity-field" key={key}>
        <label htmlFor={`identity-${key}`}>
          {label}
          {required ? " *" : ""}
        </label>
        <div className={key === "badge_id" ? "sync-badge-id-input" : undefined}>
          <input
            id={`identity-${key}`}
            name={key}
            value={draft[key]}
            type={
              key.includes("email")
                ? "email"
                : key === "net_phone"
                ? "tel"
                : "text"
            }
            onChange={(event) => change(key, event.target.value)}
            onBlur={() => {
              change(key, normalizeIdentityField(key, draft[key]));
              setTouched((old) => ({ ...old, [key]: true }));
            }}
            required={required}
            autoComplete="off"
            spellCheck={false}
            aria-invalid={Boolean(issue)}
            aria-describedby={`${hintId}${issue ? ` ${errorId}` : ""}`}
            placeholder={
              key === "badge_id" ? "acorn-river-moon-fox" : undefined
            }
          />
          {key === "badge_id" && (
            <button
              type="button"
              className="sync-secondary sync-generate-id"
              onClick={() => {
                change(key, generateBadgeId());
                setTouched((old) => ({ ...old, [key]: true }));
              }}
            >
              Generate
            </button>
          )}
        </div>
        <small id={hintId}>
          {FIELD_HINTS[key]}{" "}
          <span className="sync-byte-count">
            {new TextEncoder().encode(normalized).length} / {max} bytes
          </span>
        </small>
        {issue && (
          <span className="sync-field-error" id={errorId}>
            {issue}
          </span>
        )}
      </div>
    );
  }
  return (
    <section
      className="sync-identity-section"
      aria-labelledby="identity-heading"
    >
      <div className="sync-section-heading">
        <div>
          <p className="eyebrow">YOUR IDENTITY</p>
          <h2 id="identity-heading" tabIndex={-1}>
            Register or update your identity
          </h2>
        </div>
        <span className="sync-feature-label">
          {enabled ? "DIRECT TO YOUR BADGE" : "PREVIEW"}
        </span>
      </div>
      <div className="sync-identity-help">
        <strong>
          {record?.provisioned === false
            ? "Your badge isn’t registered yet. Start here."
            : "Badge not registered? Start here."}
        </strong>
        <p>
          Connect your badge, choose a name and badge ID, and upload your
          identity. You can do this before syncing or unlocking any apps.
        </p>
      </div>
      {!connected && (
        <p className="sync-file-note">
          Connect your badge above to load or create its identity.
        </p>
      )}
      {loadError && (
        <div className="sync-warning" role="alert">
          <p>{loadError}</p>
          <button
            className="sync-secondary"
            type="button"
            disabled={busy || !connected}
            onClick={onReload}
          >
            Reload identity
          </button>
        </div>
      )}
      <form
        noValidate
        onSubmit={(event) => {
          event.preventDefault();
          if (!enabled || locked) return;
          setTouched(
            Object.fromEntries(IDENTITY_FIELDS.map(({ key }) => [key, true]))
          );
          try {
            buildIdentity(base, draft);
            setError("");
            onSave(draft);
          } catch (issue) {
            setError(issue.message);
          }
        }}
      >
        <fieldset disabled={locked} className="sync-identity-fields">
          <legend>Badge identity</legend>
          {IDENTITY_FIELDS.filter(({ key }) => !key.startsWith("net_")).map(
            field
          )}
          <div className="sync-identity-field">
            <label htmlFor="identity-role">Role</label>
            <select
              id="identity-role"
              name="role"
              value={draft.role}
              onChange={(event) => change("role", event.target.value)}
            >
              {IDENTITY_ROLES.map(([key, label]) => (
                <option key={key} value={key}>
                  {label}
                </option>
              ))}
            </select>
          </div>
          <div className="sync-identity-color">
            <label>
              <input
                type="checkbox"
                checked={!draft.role_color}
                onChange={(event) =>
                  change("role_color", event.target.checked ? "" : "#b087b1")
                }
              />
              Use the role’s default color
            </label>
            {draft.role_color && (
              <label>
                Custom role color
                <input
                  type="color"
                  value={draft.role_color}
                  onChange={(event) => change("role_color", event.target.value)}
                />
              </label>
            )}
          </div>
        </fieldset>
        <fieldset disabled={locked} className="sync-identity-fields">
          <legend>
            Connection details{" "}
            <span>Optional · shared when you connect with another badge</span>
          </legend>
          {IDENTITY_FIELDS.filter(({ key }) => key.startsWith("net_")).map(
            field
          )}
        </fieldset>
        <div className="sync-identity-actions">
          <button
            type="submit"
            className="sync-primary"
            disabled={!enabled || locked}
            aria-busy={busy}
          >
            {busy ? "Working…" : "Upload identity to badge"}
            {busy ? (
              <span className="sync-spinner" aria-hidden="true" />
            ) : (
              <span aria-hidden="true">→</span>
            )}
          </button>
        </div>
        {error && (
          <p className="sync-warning" role="alert">
            {error}
          </p>
        )}
        {status && (
          <p className="sync-status" role="status">
            {status}
          </p>
        )}
        <p className="sync-file-note">
          {enabled
            ? "Saved directly over USB and applied without rebooting. No online sync is needed. Changing the badge ID gives you a new identity in future connections."
            : "Identity uploads are not enabled in this build."}
        </p>
      </form>
    </section>
  );
}

import React, { useState } from "react";

export default function ResetBadge({
  connected,
  busy,
  working,
  onReset,
  status,
  error,
}) {
  const [confirmed, setConfirmed] = useState(false);
  return (
    <details className="sync-reset-section">
      <summary>Reset your badge</summary>
      <div className="sync-reset-content">
        <h3>Factory reset</h3>
        <p>
          This removes your badge identity, registration, and sync credential,
          plus saved contacts, game progress, settings, and installed Lua apps.
          The badge will restart and need a new identity.
        </p>
        <p>
          Sync and download any data you want to keep first. After resetting,
          reconnect and use Register / update identity to set up your badge again.
        </p>
        {!connected && (
          <p className="sync-file-note">
            Connect your badge at the top of this page before resetting it.
          </p>
        )}
        <label className="sync-reset-confirm">
          <input
            type="checkbox"
            checked={confirmed}
            disabled={busy || !connected}
            onChange={(event) => setConfirmed(event.target.checked)}
          />
          I understand this permanently removes my badge identity, registration,
          sync credential, contacts, progress, settings, and installed apps.
        </label>
        <button
          type="button"
          className="sync-danger"
          disabled={!connected || busy || !confirmed}
          aria-busy={working}
          onClick={() => {
            setConfirmed(false);
            onReset();
          }}
        >
          {working ? "Resetting badge…" : "Factory reset badge"}
        </button>
        {status && (
          <p
            className={`sync-status${working ? " is-busy" : ""}`}
            role="status"
          >
            {working && <span className="sync-spinner" aria-hidden="true" />}
            {status}
          </p>
        )}
        {error && (
          <p className="sync-warning" role="alert">
            {error}
          </p>
        )}
      </div>
    </details>
  );
}

import React, { useEffect, useMemo, useRef, useState } from "react";
import { BadgeSerial } from "./serial.js";
import {
  contactsCsv,
  describeSnapshot,
  download,
  progressCsv,
  recordsCsv,
  visitsCsv,
} from "./data.js";
import {
  loadLastSnapshot,
  saveLastSnapshot,
  saveIdentityBackup,
} from "./storage.js";
import { uploadSnapshot } from "./supabase.js";
import { FEATURES } from "./features.js";
import FirmwareFlasher from "./FirmwareFlasher.jsx";
import IdentityEditor from "./IdentityEditor.jsx";
import ResetBadge from "./ResetBadge.jsx";
import "./sync.css";

const UNLOCK_ENABLED = FEATURES.unlock;
const pretty = (value) => JSON.stringify(value, null, 2);
function DownloadIcon() {
  return (
    <svg viewBox="0 0 20 20" aria-hidden="true">
      <path d="M10 2v11m-4-4 4 4 4-4M3 13v4h14v-4" />
    </svg>
  );
}
function SaveButton({ children, onClick, disabled = false }) {
  return (
    <button className="sync-save" onClick={onClick} disabled={disabled}>
      <DownloadIcon />
      {children}
    </button>
  );
}

function Spinner() {
  return <span className="sync-spinner" aria-hidden="true" />;
}

function OperationFeedback({ status, error, busy }) {
  return (
    <>
      {status && (
        <p className={`sync-status${busy ? " is-busy" : ""}`} role="status">
          {busy && <Spinner />}
          {status}
        </p>
      )}
      {error && (
        <p className="sync-warning" role="alert">
          {error}
        </p>
      )}
    </>
  );
}

export default function SyncPage() {
  const [saved] = useState(() => loadLastSnapshot());
  const [connected, setConnected] = useState(false);
  const [synced, setSynced] = useState(false);
  const [unlocked, setUnlocked] = useState(false);
  const [activePath, setActivePath] = useState("sync");
  const [identityRecord, setIdentityRecord] = useState(null);
  const [identityError, setIdentityError] = useState("");
  const [identityStatus, setIdentityStatus] = useState("");
  const [identityStorageError, setIdentityStorageError] = useState(false);
  const [resetStatus, setResetStatus] = useState("");
  const [resetError, setResetError] = useState("");
  const [pendingUpload, setPendingUpload] = useState(null);
  const [busy, setBusy] = useState(false);
  const [activeOperation, setActiveOperation] = useState(null);
  const [syncStatus, setSyncStatus] = useState("");
  const [syncError, setSyncError] = useState("");
  const [unlockStatus, setUnlockStatus] = useState("");
  const [unlockError, setUnlockError] = useState("");
  const [status, setStatus] = useState(
    saved
      ? "Your last badge data is saved below. Connect your badge to sync again."
      : "Ready to connect."
  );
  const [error, setError] = useState("");
  const [snapshot, setSnapshot] = useState(
    saved?.beforeUnlock ?? saved?.snapshot ?? null
  );
  const [beforeIdentity, setBeforeIdentity] = useState(
    saved?.beforeIdentity ?? null
  );
  const [source, setSource] = useState(
    saved?.beforeUnlock ? "Haker Badge · before unlock" : saved?.source ?? ""
  );
  const [readAt, setReadAt] = useState(saved?.readAt ?? null);
  const [storageError, setStorageError] = useState(false);
  const [tab, setTab] = useState("Connections");
  const [search, setSearch] = useState("");
  const [selectedFile, setSelectedFile] = useState("");
  const operation = useRef(false);
  const connection = useRef(0);
  const serial = useRef(null);
  if (!serial.current)
    serial.current = new BadgeSerial((err) => {
      connection.current++;
      setConnected(false);
      setSynced(false);
      setStatus("Badge disconnected. Reconnect to continue.");
      reportError(err.message);
      if (operation.current === "sync")
        setSyncStatus("Sync interrupted. Reconnect to try again.");
      if (operation.current === "unlock")
        setUnlockStatus("Unlock interrupted. Reconnect to try again.");
    });
  useEffect(
    () => () => {
      void serial.current.disconnect();
    },
    []
  );

  useEffect(() => {
    setStorageError(
      !saveLastSnapshot({
        snapshot,
        beforeIdentity,
        source,
        readAt,
      })
    );
  }, [snapshot, beforeIdentity, source, readAt]);
  const supported = typeof navigator !== "undefined" && "serial" in navigator;
  const data = useMemo(
    () => (snapshot ? describeSnapshot(snapshot) : null),
    [snapshot]
  );
  const filename = `badge-${String(snapshot?.badge_id || "snapshot").replace(
    /[^a-z0-9_-]/gi,
    "_"
  )}`;
  const saveCsv = (name, text) =>
    download(`${filename}-${name}.csv`, text, "text/csv;charset=utf-8");

  function reportError(message, scope = operation.current) {
    if (scope === "sync") setSyncError(message);
    else if (scope === "unlock") setUnlockError(message);
    else if (scope === "reset") setResetError(message);
    else setError(message);
  }

  function showSnapshot(doc, origin) {
    setSnapshot(doc);
    setSource(origin);
    setReadAt(new Date());
    setSearch("");
    setSelectedFile("");
  }

  async function syncToServer(doc) {
    const currentConnection = connection.current;
    setPendingUpload(doc);
    setSynced(false);
    setSyncStatus("Syncing your badge data to Hack the North…");
    try {
      await uploadSnapshot(doc);
      setPendingUpload(null);
      // An upload can finish after USB is unplugged. Keep the reconnect state.
      if (connection.current !== currentConnection) return false;
      setSynced(true);
      return true;
    } catch {
      // Network failures must keep the USB connection and local copy intact.
      if (connection.current !== currentConnection) return false;
      setSyncStatus(
        "Your badge data is available below, but hasn’t synced online."
      );
      setSyncError(
        "We couldn’t upload your badge data. Check your internet connection and click Retry sync."
      );
      return false;
    }
  }
  async function run(work, scope = "connection") {
    if (operation.current) return;
    operation.current = scope;
    setActiveOperation(scope);
    setBusy(true);
    reportError("", scope);
    try {
      await work();
    } catch (err) {
      reportError(
        err.name === "NotFoundError"
          ? "No device selected. Click Connect badge when you’re ready."
          : err.message,
        scope
      );
      if (scope === "sync")
        setSyncStatus("Sync interrupted. Reconnect to try again.");
      if (scope === "unlock")
        setUnlockStatus("Unlock interrupted. Reconnect to try again.");
      if (scope === "reset") setResetStatus("Reset interrupted.");
      // A timed-out command may still be running on the badge. Release the port
      // instead of sending another command into an unfinished transfer.
      await serial.current.disconnect();
      setConnected(false);
      setSynced(false);
      setStatus("Reconnect to try again.");
    } finally {
      operation.current = false;
      setActiveOperation(null);
      setBusy(false);
    }
  }
  function connect() {
    return run(async () => {
      setStatus("Choose USB JTAG/serial debug unit in the device picker…");
      await serial.current.connect();
      setConnected(true);
      setSynced(false);
      setUnlocked(false);
      setSyncStatus("");
      setSyncError("");
      setUnlockStatus("");
      setUnlockError("");
      connection.current++;
      setIdentityRecord(null);
      setIdentityError("");
      setIdentityStatus("");
      setResetStatus("");
      setResetError("");
      setPendingUpload(null);
      // Data on screen must always belong to the newly selected badge.
      setSnapshot(null);
      setBeforeIdentity(null);
      setSource("");
      const record = await readConnectedIdentity();
      setActivePath(record?.provisioned ? "sync" : "identity");
      setStatus(
        record?.provisioned
          ? "Badge connected. Sync your data to get started."
          : record
          ? "Badge connected. Register your identity to get started."
          : "Badge connected. Check the identity section before continuing."
      );
    });
  }
  function sync() {
    return run(async () => {
      if (!connected) return;
      setSyncStatus("Reading your badge data… Keep it plugged in.");
      // Retry the received data after a network failure without restarting the badge.
      const doc =
        pendingUpload || (await serial.current.snapshot({ restart: false }));
      showSnapshot(doc, "Haker Badge");
      if (doc.provisioned === false) {
        setActivePath("identity");
        setSyncStatus(
          "This badge isn’t registered. Upload an identity first, then return to Sync & unlock."
        );
        return;
      }
      if (await syncToServer(doc))
        setSyncStatus(
          unlocked
            ? "Badge data synced. Your apps are already unlocked."
            : "Badge data synced to Hack the North. You can now unlock all apps."
        );
    }, "sync");
  }
  function unlock() {
    if (!UNLOCK_ENABLED || !connected || !synced || unlocked) return;
    return run(async () => {
      await serial.current.unlock(snapshot, setUnlockStatus);
      setUnlocked(true);
      setUnlockStatus("Badge data synced and all public apps unlocked.");
    }, "unlock");
  }
  async function readConnectedIdentity() {
    setIdentityError("");
    setIdentityStatus("Loading your badge identity…");
    try {
      const record = await serial.current.loadIdentity();
      setIdentityRecord(record);
      setIdentityStatus(
        record.provisioned
          ? "Your current identity is loaded."
          : "This badge isn’t registered yet. Fill in your identity below."
      );
      return record;
    } catch (error) {
      setIdentityRecord(null);
      setIdentityError(error.message);
      setIdentityStatus("");
      return null;
    }
  }
  function saveIdentity(draft) {
    if (!FEATURES.identity || !connected || !identityRecord) return;
    return run(async () => {
      const record = await serial.current.saveIdentity(
        identityRecord,
        draft,
        (backup) => setIdentityStorageError(!saveIdentityBackup(backup)),
        (message) => {
          setIdentityStatus(message);
          setStatus(message);
        }
      );
      setIdentityRecord(record);
      setIdentityError("");
      // This USB-only branch does not depend on, or silently trigger, uploads.
      // Any older pending snapshot must not be reused under the new identity.
      setSynced(false);
      setPendingUpload(null);
      setIdentityStatus(
        "Identity uploaded and applied. Your badge is ready — no restart needed."
      );
      setStatus(
        "Identity updated. You can sync your badge data whenever you’re ready."
      );
    });
  }
  function resetBadge() {
    if (!connected) return;
    return run(async () => {
      await serial.current.factoryReset(true, setResetStatus);
      connection.current++;
      setConnected(false);
      setSynced(false);
      setUnlocked(false);
      setPendingUpload(null);
      setIdentityRecord(null);
      setIdentityError("");
      setIdentityStatus("");
      setResetStatus(
        "Factory reset complete. Your badge identity, registration, sync credential, and event data were removed. Your Solana wallet key was preserved. Reconnect and use Register / update identity to set up your badge again."
      );
      setStatus("Badge reset. Connect again to continue.");
    }, "reset");
  }

  const contacts = (data?.contacts || []).filter((contact) =>
    `${contact.name} ${contact.role} ${contact.details
      .map((d) => d.value)
      .join(" ")}`
      .toLowerCase()
      .includes(search.toLowerCase())
  );
  const games = (data?.summary?.games || []).filter(
    (game) => game.metrics.length
  );
  const fileEntry = data?.files.find((file) => file.path === selectedFile);

  return (
    <main id="content" className="sync-page">
      <section className="sync-intro">
        <p className="eyebrow">HACK THE NORTH 2026</p>
        <h1>Sync your badge.</h1>
        <p>Save your badge data. Unlock all apps. Keep exploring.</p>
      </section>

      <section className="sync-connect-panel" aria-labelledby="connect-heading">
        <div className="sync-connect-main">
          <p className="eyebrow">01 · CONNECT FIRST</p>
          <h2 id="connect-heading">
            {connected ? "Your badge is connected." : "Connect your badge."}
          </h2>
          <p className="sync-instruction">
            Switch your badge <strong>OFF</strong> and plug it into your
            computer with a USB data cable. Choose{" "}
            <strong>USB JTAG/serial debug unit</strong> or{" "}
            <strong>Espressif</strong> in the device picker.
          </p>
          <p className="sync-browser-note">
            Use Chrome or Edge on your computer. Safari isn’t supported.
          </p>
          <div className="sync-actions">
            <button
              className="sync-primary"
              onClick={connect}
              disabled={connected || busy || !supported}
              aria-busy={activeOperation === "connection"}
            >
              {connected
                ? "Badge connected"
                : activeOperation === "connection"
                ? "Connecting…"
                : "Connect badge"}
              {activeOperation === "connection" ? (
                <Spinner />
              ) : (
                <span aria-hidden="true">{connected ? "✓" : "→"}</span>
              )}
            </button>
            {connected && (
              <button
                className="sync-disconnect"
                disabled={busy}
                onClick={() =>
                  run(async () => {
                    await serial.current.disconnect();
                    connection.current++;
                    setConnected(false);
                    setSynced(false);
                    setStatus(
                      "Badge disconnected. Your saved data is available below."
                    );
                  })
                }
              >
                Disconnect
              </button>
            )}
          </div>
          <p
            className={`sync-status${
              activeOperation === "connection" ? " is-busy" : ""
            }`}
            role="status"
          >
            {activeOperation === "connection" ? (
              <Spinner />
            ) : (
              <span className={`sync-dot${connected ? " connected" : ""}`} />
            )}
            {status}
          </p>
          {error && (
            <p className="sync-warning" role="alert">
              {error}
            </p>
          )}
          {!supported && (
            <p className="sync-warning" role="alert">
              USB connection needs Chrome or Edge on a computer.
            </p>
          )}
          <details className="sync-help">
            <summary>Having trouble connecting?</summary>
            <p>
              Use a cable that carries data, keep the badge switch OFF, and
              close the Badge IDE and any other tabs or apps connected to your
              badge. Then try again.
            </p>
          </details>
        </div>
      </section>

      <div
        className="sync-path-picker"
        aria-label="Choose what to do after connecting"
      >
        <button
          type="button"
          aria-pressed={activePath === "sync"}
          onClick={() => setActivePath("sync")}
          aria-controls="sync-workflow"
        >
          <strong>Sync &amp; unlock</strong>
          <span>Save your event data, then unlock every public app.</span>
          <span aria-hidden="true">→</span>
        </button>
        <button
          type="button"
          aria-pressed={activePath === "identity"}
          onClick={() => setActivePath("identity")}
          aria-controls="identity-workflow"
        >
          <strong>Register / update identity</strong>
          <span>
            Set up an unregistered badge or change your details. No sync
            required.
          </span>
          <span aria-hidden="true">→</span>
        </button>
      </div>

      <section
        id="sync-workflow"
        className="sync-workflow"
        hidden={activePath !== "sync"}
        aria-labelledby="workflow-heading"
      >
        <h2 id="workflow-heading">Sync your data. Unlock your apps.</h2>
        <p className="sync-file-note">
          Already registered? Follow these two steps after connecting. If your
          badge isn’t registered, use Register / update identity first.
        </p>
        <div className="sync-workflow-steps">
          <article>
            <p className="eyebrow">02 · SYNC</p>
            <h3>Save your badge data</h3>
            <p>
              Sync your contacts, scores, and progress to Hack the North, then
              download your own copy below.
            </p>
            <button
              className="sync-primary"
              onClick={sync}
              disabled={
                !connected || busy || identityRecord?.provisioned === false
              }
              aria-busy={activeOperation === "sync"}
            >
              {activeOperation === "sync"
                ? "Syncing…"
                : pendingUpload
                ? "Retry sync"
                : synced
                ? "Sync again"
                : "Sync badge data"}
              {activeOperation === "sync" ? (
                <Spinner />
              ) : (
                <span aria-hidden="true">→</span>
              )}
            </button>
            {identityRecord?.provisioned === false && (
              <p className="sync-file-note">
                Register your badge using the identity tab first.
              </p>
            )}
            <OperationFeedback
              status={syncStatus}
              error={syncError}
              busy={activeOperation === "sync"}
            />
          </article>
          <article>
            <p className="eyebrow">03 · UNLOCK</p>
            <h3>Unlock all apps</h3>
            <p>
              Keep your connections and progress, and take every public app home
              with you.
            </p>
            <button
              className="sync-primary"
              onClick={unlock}
              disabled={
                !connected || !synced || busy || unlocked || !UNLOCK_ENABLED
              }
              aria-busy={activeOperation === "unlock"}
            >
              {activeOperation === "unlock"
                ? "Unlocking…"
                : unlocked
                ? "All apps unlocked"
                : "Unlock all apps"}
              {activeOperation === "unlock" ? (
                <Spinner />
              ) : (
                <span aria-hidden="true">{unlocked ? "✓" : "→"}</span>
              )}
            </button>
            {!synced && (
              <p className="sync-file-note">
                Sync your data first to keep a backup.
              </p>
            )}
            {!UNLOCK_ENABLED && (
              <p className="sync-file-note">
                Unlocking is not enabled in this build.
              </p>
            )}
            <OperationFeedback
              status={unlockStatus}
              error={unlockError}
              busy={activeOperation === "unlock"}
            />
          </article>
        </div>
      </section>
      <div id="identity-workflow" hidden={activePath !== "identity"}>
        <IdentityEditor
          key={`${connection.current}:${
            identityRecord?.raw ?? (identityRecord ? "unregistered" : "loading")
          }`}
          record={identityRecord}
          connected={connected}
          busy={busy}
          enabled={FEATURES.identity}
          onSave={saveIdentity}
          onReload={() => run(readConnectedIdentity)}
          loadError={identityError}
          status={identityStatus}
        />
        {identityStorageError && (
          <p className="sync-warning" role="alert">
            Your browser couldn’t store a recovery copy of the previous
            identity.
          </p>
        )}
      </div>

      {data && (
        <section className="sync-data-section" aria-labelledby="data-heading">
          <div className="sync-section-heading">
            <div>
              <h2 id="data-heading">Your badge data</h2>
            </div>
          </div>
          {storageError && (
            <p className="sync-warning" role="alert">
              Your browser couldn’t save this snapshot. You can still download
              your files.
            </p>
          )}
          <div className="sync-receipt">
            <div>
              <span className="sync-source">{source}</span>
              <h3>
                {typeof data.identity?.display_name === "string"
                  ? data.identity.display_name
                  : "Your badge"}
              </h3>
              <p>
                Badge {String(snapshot.badge_id || "unregistered")} ·{" "}
                {source === "Imported JSON" ? "Opened" : "Read"}{" "}
                {readAt?.toLocaleTimeString([], {
                  hour: "2-digit",
                  minute: "2-digit",
                })}
              </p>
            </div>
            <SaveButton
              onClick={() => download(`${filename}.json`, pretty(snapshot))}
            >
              Download full JSON
            </SaveButton>
          </div>
          {beforeIdentity && (
            <div className="sync-backup">
              <p>
                Your before-edit snapshot keeps the identity that was on your
                badge before the last change.
              </p>
              <SaveButton
                onClick={() =>
                  download(
                    "badge-before-identity-edit.json",
                    pretty(beforeIdentity)
                  )
                }
              >
                Before-edit JSON
              </SaveButton>
            </div>
          )}
          <div className="sync-stats">
            <div>
              <strong>{data.contacts.length}</strong>
              <span>Connections</span>
            </div>
            <div>
              <strong>
                {data.summary?.percent ?? "-"}
                <small>%</small>
              </strong>
              <span>Event progress</span>
            </div>
            <div>
              <strong>{games.length}</strong>
              <span>Games with records</span>
            </div>
            <div>
              <strong>{data.files.length}</strong>
              <span>Snapshot files</span>
            </div>
          </div>
          <div className="sync-tabs" role="tablist" aria-label="Badge data">
            {["Connections", "Progress", "Files", "JSON"].map((name) => (
              <button
                key={name}
                id={`tab-${name}`}
                role="tab"
                aria-selected={tab === name}
                aria-controls="sync-tab-panel"
                tabIndex={tab === name ? 0 : -1}
                onClick={() => setTab(name)}
                onKeyDown={(event) => {
                  const names = ["Connections", "Progress", "Files", "JSON"];
                  let next;
                  if (event.key === "ArrowRight")
                    next = (names.indexOf(tab) + 1) % 4;
                  if (event.key === "ArrowLeft")
                    next = (names.indexOf(tab) + 3) % 4;
                  if (event.key === "Home") next = 0;
                  if (event.key === "End") next = 3;
                  if (next !== undefined) {
                    event.preventDefault();
                    setTab(names[next]);
                    document.getElementById(`tab-${names[next]}`).focus();
                  }
                }}
              >
                {name}
                {name === "Connections" && <span>{data.contacts.length}</span>}
              </button>
            ))}
          </div>
          <div
            id="sync-tab-panel"
            role="tabpanel"
            aria-labelledby={`tab-${tab}`}
            tabIndex={0}
          >
            {tab === "Connections" && (
              <>
                <div className="sync-toolbar">
                  <label>
                    Find a connection
                    <input
                      type="search"
                      value={search}
                      onChange={(event) => setSearch(event.target.value)}
                      placeholder="Name, email, or handle"
                    />
                  </label>
                  <SaveButton
                    onClick={() =>
                      saveCsv("connections", contactsCsv(data.contacts))
                    }
                  >
                    Connections CSV
                  </SaveButton>
                </div>
                <div className="sync-contact-grid">
                  {contacts.map((contact) => (
                    <article className="sync-contact" key={contact.badgeId}>
                      <div className="sync-contact-head">
                        <span className="sync-avatar">
                          {Array.from(contact.name)[0]}
                        </span>
                        <div>
                          <h3>{contact.name}</h3>
                          <p>{contact.role}</p>
                        </div>
                      </div>
                      <dl>
                        {contact.details.map((detail) => (
                          <div key={detail.label}>
                            <dt>{detail.label}</dt>
                            <dd>
                              {detail.href ? (
                                <a
                                  href={detail.href}
                                  target="_blank"
                                  rel="noreferrer"
                                >
                                  {detail.value} ↗
                                </a>
                              ) : (
                                detail.value
                              )}
                            </dd>
                          </div>
                        ))}
                      </dl>
                    </article>
                  ))}
                </div>
                {!contacts.length && (
                  <p className="sync-no-data">
                    {search
                      ? "No connections match your search."
                      : "No saved connections in this snapshot yet."}
                  </p>
                )}
              </>
            )}
            {tab === "Progress" && (
              <>
                <div className="sync-toolbar">
                  <h3>Explore. Play. Collect.</h3>
                  <SaveButton
                    onClick={() =>
                      saveCsv("progress", progressCsv(data.summary))
                    }
                  >
                    Progress CSV
                  </SaveButton>
                </div>
                <div className="sync-progress-grid">
                  {data.summary?.progress?.map((row) => (
                    <article key={row.label}>
                      <p>{row.label}</p>
                      <strong>
                        {row.count}
                        <small> / {row.total}</small>
                      </strong>
                      <progress
                        aria-label={row.label}
                        max={row.total}
                        value={Math.min(row.count, row.total)}
                      />
                    </article>
                  ))}
                </div>
                <div className="sync-toolbar">
                  <h3>Your personal bests</h3>
                  <SaveButton
                    onClick={() =>
                      saveCsv("game-records", recordsCsv(data.summary))
                    }
                  >
                    Game records CSV
                  </SaveButton>
                </div>
                <div className="sync-game-grid">
                  {games.map((game) => (
                    <article key={game.name}>
                      <h4>{game.name}</h4>
                      {game.metrics.map((metric) => (
                        <div key={metric.label}>
                          <span>{metric.label}</span>
                          <strong>
                            {metric.value.toLocaleString()} {metric.unit}
                          </strong>
                        </div>
                      ))}
                    </article>
                  ))}
                </div>
                {!games.length && (
                  <p className="sync-no-data">
                    No saved game records in this snapshot.
                  </p>
                )}
                <div className="sync-toolbar">
                  <h3>
                    Sponsor & workshop visits{" "}
                    <span>({data.visits.length})</span>
                  </h3>
                  <SaveButton
                    disabled={!data.visits.length}
                    onClick={() => saveCsv("visits", visitsCsv(data.visits))}
                  >
                    Visits CSV
                  </SaveButton>
                </div>
                <div className="sync-visits">
                  {data.visits.map((visit) => (
                    <div key={visit.path}>
                      <span>{visit.kind}</span>
                      <strong>
                        {visit.name || visit.title || visit.slug || visit.path}
                      </strong>
                    </div>
                  ))}
                </div>
              </>
            )}
            {tab === "Files" && (
              <>
                <div className="sync-toolbar">
                  <h3>Everything the badge returned</h3>
                  <span>
                    {(
                      new TextEncoder().encode(pretty(snapshot)).length / 1024
                    ).toFixed(1)}{" "}
                    KB JSON
                  </span>
                </div>
                <p className="sync-file-note">
                  Select a file to inspect it. Skipped or truncated files are
                  marked; unknown app data is preserved in the full JSON.
                </p>
                <div className="sync-file-layout">
                  <div className="sync-file-list">
                    {data.files.map(({ path, entry }) => (
                      <button
                        className={selectedFile === path ? "selected" : ""}
                        key={path}
                        onClick={() => setSelectedFile(path)}
                      >
                        <span>{path}</span>
                        <small>
                          {entry?.skip ||
                            (entry?.trunc
                              ? "truncated"
                              : `${entry?.size ?? "?"} B`)}
                        </small>
                      </button>
                    ))}
                  </div>
                  <div className="sync-file-view">
                    {fileEntry ? (
                      <>
                        <div className="sync-toolbar">
                          <strong>{fileEntry.path}</strong>
                          <SaveButton
                            disabled={typeof fileEntry.entry?.text !== "string"}
                            onClick={() =>
                              download(
                                fileEntry.path +
                                  (fileEntry.entry?.trunc
                                    ? ".partial.txt"
                                    : ""),
                                fileEntry.entry.text,
                                "text/plain;charset=utf-8"
                              )
                            }
                          >
                            Save file
                          </SaveButton>
                        </div>
                        <pre>
                          {fileEntry.entry?.text ??
                            `Contents not returned: ${
                              fileEntry.entry?.skip || "unavailable"
                            }`}
                        </pre>
                      </>
                    ) : (
                      <p>Select a file to see its contents.</p>
                    )}
                  </div>
                </div>
              </>
            )}
            {tab === "JSON" && (
              <>
                <div className="sync-toolbar">
                  <h3>The original snapshot, formatted.</h3>
                  <SaveButton
                    onClick={() =>
                      download(`${filename}.json`, pretty(snapshot))
                    }
                  >
                    Full JSON
                  </SaveButton>
                </div>
                <p className="sync-file-note">
                  The full export can include personal contact information and
                  wallet data. Keep your backup somewhere private.
                </p>
                <pre className="sync-json">{pretty(snapshot)}</pre>
              </>
            )}
          </div>
        </section>
      )}
      <details className="sync-flasher-section">
        <summary>Need to install or reinstall your badge firmware?</summary>
        <p>
          Reinstall the official firmware here. Download your badge data first,
          then disconnect above before continuing.
        </p>
        <FirmwareFlasher portBusy={connected || busy} />
      </details>
      <ResetBadge
        key={connected ? "connected" : "disconnected"}
        connected={connected}
        busy={busy}
        working={activeOperation === "reset"}
        onReset={resetBadge}
        status={resetStatus}
        error={resetError}
      />
    </main>
  );
}

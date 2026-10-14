import React, { useEffect, useRef, useState } from "react";
import {
  CONNECT_TIMEOUT_MS,
  ESPTOOL_BUNDLE_URL,
  FIRMWARE_MANIFEST_URL,
  connectTimeoutError,
  flashPlan,
  overallProgress,
  withTimeout,
} from "./firmware.js";

// In-browser flasher for the official badge image set: picks the badge's
// serial port, flashes the committed /firmware binaries with esptool-js,
// then reboots. Same flow as the factory web flasher (tools/web-flasher),
// pointed at the official image instead of the factory-test one.
async function fetchBinaryString(url) {
  const res = await fetch(url, { cache: "no-store" });
  if (!res.ok) throw new Error(`Couldn't load ${url} (${res.status})`);
  const bytes = new Uint8Array(await res.arrayBuffer());
  let out = "";
  const CHUNK = 0x8000;
  for (let i = 0; i < bytes.length; i += CHUNK) {
    out += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
  }
  return out;
}

export default function FirmwareFlasher({ portBusy }) {
  const [manifest, setManifest] = useState(null);
  const [missing, setMissing] = useState(false);
  const [phase, setPhase] = useState("idle");
  const [status, setStatus] = useState("");
  const [progress, setProgress] = useState(0);
  const [error, setError] = useState("");
  const runId = useRef(0);
  const transportRef = useRef(null);
  const supported =
    typeof navigator !== "undefined" && "serial" in navigator;

  useEffect(() => {
    let cancelled = false;
    fetch(FIRMWARE_MANIFEST_URL, { cache: "no-store" })
      .then((res) => {
        if (!res.ok) throw new Error(`firmware manifest: ${res.status}`);
        return res.json();
      })
      .then((doc) => {
        if (!cancelled) setManifest(doc);
      })
      .catch(() => {
        if (!cancelled) setMissing(true);
      });
    return () => {
      cancelled = true;
    };
  }, []);

  function cancel() {
    runId.current += 1;
    void transportRef.current?.disconnect().catch(() => {});
    transportRef.current = null;
    setPhase("idle");
    setStatus("");
    setProgress(0);
  }

  async function flash() {
    const myRun = runId.current + 1;
    runId.current = myRun;
    const alive = () => runId.current === myRun;
    setError("");
    try {
      const plan = flashPlan(manifest);
      const port = await navigator.serial.requestPort();
      if (!alive()) return;
      setPhase("working");
      setProgress(0);
      setStatus("Connecting…");
      // Runtime-only import: the bundle lives in public/ (copied as-is, no
      // plugin transforms), so source code may not import it, not even with
      // @vite-ignore, which dev still tries to resolve. The Function
      // constructor keeps the specifier opaque to Vite; the browser loads it
      // natively at flash time.
      const runtimeImport = new Function("url", "return import(url)");
      const connect = (async () => {
        const { ESPLoader, Transport, UsbJtagSerialReset } =
          await runtimeImport(ESPTOOL_BUNDLE_URL);
        const transport = new Transport(port, true);
        transportRef.current = transport;
        const loader = new ESPLoader({
          transport,
          baudrate: 460800,
          romBaudrate: 115200,
        });
        await loader.main();
        return { transport, loader, UsbJtagSerialReset };
      })();
      // A badge that never answers the bootloader sync (running app, wrong
      // port, another tab holding it) must surface recovery steps, not spin.
      const { transport, loader, UsbJtagSerialReset } = await withTimeout(
        connect,
        CONNECT_TIMEOUT_MS,
        connectTimeoutError
      );
      if (!alive()) {
        await transport.disconnect().catch(() => {});
        return;
      }
      setStatus("Loading firmware…");
      const fileArray = [];
      for (const part of plan) {
        fileArray.push({
          data: await fetchBinaryString(part.url),
          address: part.address,
        });
      }
      const sizes = fileArray.map((file) => file.data.length);
      setStatus("Flashing… do not unplug.");
      await loader.writeFlash({
        fileArray,
        flashSize: "keep",
        flashMode: "keep",
        flashFreq: "keep",
        eraseAll: false,
        compress: true,
        reportProgress: (index, written) =>
          setProgress(overallProgress(sizes, index, written)),
      });
      setProgress(100);
      // Native USB-Serial-JTAG re-enumerates mid-reset, so the tail of
      // reset() throws "device lost"; that is the reboot, not a failure.
      setStatus("Restarting…");
      try {
        await new UsbJtagSerialReset(transport).reset();
      } catch (err) {
        console.debug("reset ended early (re-enumeration, expected):", err);
      }
      if (!alive()) return;
      setPhase("success");
      setStatus("");
    } catch (err) {
      if (!alive()) return;
      // Cancelling the port picker returns to idle without an error.
      if (
        err &&
        (err.name === "NotFoundError" ||
          /No port selected/i.test(err.message || ""))
      ) {
        setPhase("idle");
        setStatus("");
      } else {
        console.error(err);
        setError((err && err.message) || String(err));
        setPhase("error");
      }
    } finally {
      const transport = transportRef.current;
      transportRef.current = null;
      try {
        await transport?.disconnect();
      } catch (_) {
        // The reboot already dropped the port; nothing to release.
      }
    }
  }

  if (missing) return <span className="sync-coming-soon">Coming soon</span>;
  if (!manifest)
    return <p className="sync-file-note">Loading official firmware…</p>;

  const plan = (() => {
    try {
      return flashPlan(manifest);
    } catch {
      return null;
    }
  })();

  return (
    <div className="sync-firmware">
      <p className="sync-firmware-version">
        {manifest.label} · {(manifest.totalBytes / 1048576).toFixed(1)} MB ·{" "}
        <code title={manifest.identity}>
          {manifest.identity?.slice(0, 12)}…
        </code>
      </p>
      {phase === "working" && (
        <div className="sync-flash-progress" role="status">
          <div className="sync-flash-track">
            <div
              className="sync-flash-fill"
              style={{ width: `${progress}%` }}
            />
          </div>
          <p>
            {status} {progress}%
          </p>
          <button className="sync-secondary" onClick={cancel}>
            Cancel
          </button>
        </div>
      )}
      {phase === "success" && (
        <p className="sync-flash-success" role="status">
          Flashed. Your badge is restarting into the official firmware. Connect
          again and use Register / update identity if your badge needs an identity.
        </p>
      )}
      {phase === "error" && (
        <p className="sync-warning" role="alert">
          Flash failed: {error} Your previous firmware is untouched until a
          write succeeds. Try again.
        </p>
      )}
      {(phase === "idle" || phase === "error") && (
        <>
          <button
            className="sync-primary sync-flash-button"
            disabled={
              !supported || portBusy || !plan || phase === "working"
            }
            onClick={flash}
          >
            Flash official firmware <span aria-hidden="true">↗</span>
          </button>
          {!supported ? (
            <p className="sync-warning">
              Flashing needs desktop Chrome or Edge with Web Serial.
            </p>
          ) : portBusy ? (
            <p className="sync-file-note">
              Disconnect your badge in section 01 first: the flasher needs
              the serial port.
            </p>
          ) : (
            <p className="sync-file-note">
              Switch the badge OFF, plug it in, then click. Keep it connected
              until the success message. If connecting stalls past ~45
              seconds, you&rsquo;ll get download-mode steps; the fastest fix
              is usually holding Start while plugging in USB, then flashing
              again.
            </p>
          )}
          <details className="sync-help">
            <summary>Advanced: flash manually</summary>
            <ul className="sync-firmware-files">
              {plan?.map((file) => (
                <li key={file.name}>
                  <a href={file.url} download={file.name}>
                    {file.name}
                  </a>
                  <span>
                    0x{file.address.toString(16)}
                  </span>
                </li>
              ))}
            </ul>
            <pre className="sync-firmware-command">
              esptool.py -p /dev/tty.usbmodem* -b 460800 --before
              default_reset --after hard_reset --chip esp32c3 write_flash
              --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0
              bootloader.bin 0x8000 partition-table.bin 0x10000 firmware.bin
            </pre>
          </details>
        </>
      )}
      {phase === "success" && (
        <button
          className="sync-secondary"
          onClick={() => {
            setPhase("idle");
            setProgress(0);
          }}
        >
          Flash another badge
        </button>
      )}
    </div>
  );
}

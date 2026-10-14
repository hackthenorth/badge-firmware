//! rapid-flash: hotplug-triggered ESP32-C3 firmware flasher.
//!
//! Watches the serial-port list and, the moment a matching USB device appears,
//! flashes it with a preconfigured firmware image set. Each board flashes in
//! its own thread, so plugging a second board mid-flash just works.
//!
//! Safety model (avoid bricking the wrong thing):
//!   1. USB VID:PID prefilter (default: Espressif native USB-JTAG `303a:1001`).
//!   2. Mandatory chip detect: flashing aborts unless the attached chip
//!      identifies as ESP32-C3 (checked inside espflash before any erase).
//!   3. Exactly-once per plug-in: a board is never reflashed while it stays
//!      connected; unplug + replug re-arms it.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{Context, Result, bail};
use clap::Parser;
use serialport::{SerialPortType, UsbPortInfo};
use sha2::{Digest, Sha256};

use espflash::connection::{Connection, ResetAfterOperation, ResetBeforeOperation};
use espflash::flasher::Flasher;
use espflash::image_format::Segment;
use espflash::target::{Chip, ProgressCallbacks};

/// Espressif native USB-Serial-JTAG (what the badge exposes).
const DEFAULT_VID_PID: &str = "303a:1001";

/// Firmware baked into the binary at compile time from `firmware/` (the
/// checked-in copy of `build/flasher_args.json` plus its bins), so the tool
/// runs standalone with no arguments and no sidecar files. Rebuilt — and the
/// mosaic/hash below changes — whenever those files change.
const EMBEDDED_FLASHER_ARGS: &str = include_str!("../firmware/flasher_args.json");
const EMBEDDED_BOOTLOADER: &[u8] = include_bytes!("../firmware/bootloader/bootloader.bin");
const EMBEDDED_PART_TABLE: &[u8] =
    include_bytes!("../firmware/partition_table/partition-table.bin");
const EMBEDDED_APP: &[u8] = include_bytes!("../firmware/hello_world.bin");

/// Relay image set (tools/relay_firmware build output, copied here).
const EMBEDDED_RELAY_ARGS: &str = include_str!("../firmware/relay/flasher_args.json");
const EMBEDDED_RELAY_BOOTLOADER: &[u8] =
    include_bytes!("../firmware/relay/bootloader/bootloader.bin");
const EMBEDDED_RELAY_PART_TABLE: &[u8] =
    include_bytes!("../firmware/relay/partition_table/partition-table.bin");
const EMBEDDED_RELAY_APP: &[u8] = include_bytes!("../firmware/relay/relay_firmware.bin");

/// Sync-station image set (tools/sync_station build output, copied here).
const EMBEDDED_SYNC_ARGS: &str = include_str!("../firmware/sync/flasher_args.json");
const EMBEDDED_SYNC_BOOTLOADER: &[u8] =
    include_bytes!("../firmware/sync/bootloader/bootloader.bin");
const EMBEDDED_SYNC_PART_TABLE: &[u8] =
    include_bytes!("../firmware/sync/partition_table/partition-table.bin");
const EMBEDDED_SYNC_APP: &[u8] = include_bytes!("../firmware/sync/sync_station.bin");

#[derive(Parser, Debug)]
#[command(name = "rapid-flash", version, about = "Flash ESP32-C3 boards the instant they are plugged in.")]
struct Args {
    /// Firmware part to flash, as OFFSET:PATH (e.g. 0x10000:app.bin).
    /// Repeat for bootloader / partition table / app.
    #[arg(long = "part", value_name = "OFFSET:PATH")]
    parts: Vec<String>,

    /// ESP-IDF `flasher_args.json` (from `idf.py build`); its flash_files
    /// map is used as the part list (paths resolve relative to the file).
    #[arg(long = "flasher-args", value_name = "PATH")]
    flasher_args: Option<PathBuf>,

    /// Flash the baked-in relay firmware set instead of the badge default.
    #[arg(long, conflicts_with_all = ["sync", "parts", "flasher_args"])]
    relay: bool,

    /// Flash the baked-in sync-station firmware set instead of the badge
    /// default.
    #[arg(long, conflicts_with_all = ["relay", "parts", "flasher_args"])]
    sync: bool,

    /// Baud rate used for flashing (connect always starts at 115200).
    #[arg(long, default_value_t = 460800)]
    baud: u32,

    /// USB VID:PID prefilter, as hex `vvvv:pppp`. Repeatable.
    #[arg(long = "vid-pid", value_name = "VID:PID", default_value = DEFAULT_VID_PID)]
    vid_pid: Vec<String>,

    /// Skip the VID:PID prefilter and probe every newly-appearing USB serial
    /// port. The ESP32-C3 chip check still applies.
    #[arg(long, default_value_t = false)]
    allow_any_usb: bool,

    /// Skip the ESP32-C3 chip gate (DANGEROUS: any ESP chip may be flashed).
    /// Refusals are reported, never flashed, unless this is passed.
    #[arg(long, default_value_t = false)]
    allow_any_chip: bool,

    // NOTE: post-write verification is unconditional by design (production
    // policy: never ship an unverified board). There is deliberately no
    // flag to disable it.

    /// How often to rescan the serial-port list.
    #[arg(long, default_value_t = 150, value_name = "MS")]
    poll_ms: u64,

    /// Delay after a port appears before opening it (lets the OS settle).
    #[arg(long, default_value_t = 400, value_name = "MS")]
    settle_ms: u64,

    /// Extra flash attempts per plug-in after the first failure.
    #[arg(long, default_value_t = 1)]
    retries: u32,

    /// Leave boards that are already plugged in at startup alone; only
    /// flash new arrivals.
    #[arg(long, default_value_t = false)]
    no_initial_scan: bool,

    /// Flash matching boards, then exit (no hotplug monitoring).
    #[arg(long, default_value_t = false)]
    once: bool,

    /// Detect and report only; never open or flash a port.
    #[arg(long, default_value_t = false)]
    dry_run: bool,

    /// Append one CSV row per flash attempt.
    #[arg(long, value_name = "PATH")]
    log_csv: Option<PathBuf>,
}

/// One firmware image loaded in memory, ready to write at `addr`.
#[derive(Debug, Clone)]
struct Part {
    addr: u32,
    path: PathBuf,
    data: Vec<u8>,
}

/// A USB serial port snapshot used for filtering and identity.
#[derive(Debug, Clone)]
struct PortView {
    port_name: String,
    vid: u16,
    pid: u16,
    serial: Option<String>,
}

impl PortView {
    /// Stable identity across re-enumeration: USB serial (== MAC on ESP
    /// parts) when present, else the port name.
    fn identity(&self) -> String {
        match &self.serial {
            Some(s) if !s.is_empty() => format!("usb:{:04x}:{:04x}:{s}", self.vid, self.pid),
            _ => format!("port:{}", self.port_name),
        }
    }

    fn short_id(&self) -> String {
        match &self.serial {
            Some(s) if !s.is_empty() => s.clone(),
            _ => self.port_name.clone(),
        }
    }
}

fn parse_u32(s: &str) -> Result<u32> {
    let s = s.trim();
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        Ok(u32::from_str_radix(hex, 16).with_context(|| format!("bad hex address '{s}'"))?)
    } else {
        Ok(s.parse::<u32>().with_context(|| format!("bad address '{s}'"))?)
    }
}

fn parse_part(spec: &str) -> Result<(u32, PathBuf)> {
    let (addr_s, path_s) = split_offset_path(spec)?;
    Ok((parse_u32(addr_s)?, PathBuf::from(path_s)))
}

/// Split `OFFSET:PATH`, tolerating a `C:\` drive-letter colon on Windows.
fn split_offset_path(spec: &str) -> Result<(&str, &str)> {
    let idx = spec
        .find(':')
        .ok_or_else(|| anyhow::anyhow!("--part must be OFFSET:PATH, got '{spec}'"))?;
    let (mut addr_s, mut path_s) = spec.split_at(idx);
    path_s = &path_s[1..];
    if cfg!(windows) && path_s.len() >= 2 && path_s.as_bytes()[1] == b':' {
        // `0x0:C:\fw.bin` already split correctly; `C:\fw.bin` alone has no
        // offset and fails cleanly below.
        let _ = addr_s;
    }
    addr_s = addr_s.trim();
    if addr_s.is_empty() || path_s.trim().is_empty() {
        bail!("--part must be OFFSET:PATH, got '{spec}'");
    }
    Ok((addr_s, path_s.trim()))
}

fn parse_vid_pid(s: &str) -> Result<(u16, u16)> {
    let (v, p) = s
        .split_once(':')
        .ok_or_else(|| anyhow::anyhow!("--vid-pid must be VID:PID, got '{s}'"))?;
    let vid = u16::from_str_radix(v.trim().trim_start_matches("0x"), 16)
        .with_context(|| format!("bad VID in '{s}'"))?;
    let pid = u16::from_str_radix(p.trim().trim_start_matches("0x"), 16)
        .with_context(|| format!("bad PID in '{s}'"))?;
    Ok((vid, pid))
}

/// Load `build/flasher_args.json` style files: `{ "flash_files": { "0x0": "path", ... } }`.
fn load_flasher_args(json_path: &Path) -> Result<Vec<(u32, PathBuf)>> {
    let text =
        fs::read_to_string(json_path).with_context(|| format!("reading {}", json_path.display()))?;
    let v: serde_json::Value =
        serde_json::from_str(&text).with_context(|| format!("parsing {}", json_path.display()))?;
    let files = v
        .get("flash_files")
        .and_then(|f| f.as_object())
        .ok_or_else(|| anyhow::anyhow!("{} has no flash_files map", json_path.display()))?;
    let base = json_path.parent().unwrap_or_else(|| Path::new("."));
    let mut out = Vec::new();
    for (addr_s, path_v) in files {
        let path_s = path_v
            .as_str()
            .ok_or_else(|| anyhow::anyhow!("non-string path for offset {addr_s}"))?;
        out.push((parse_u32(addr_s)?, base.join(path_s)));
    }
    out.sort_by_key(|(addr, _)| *addr);
    Ok(out)
}

/// Parts compiled into the binary (offsets from the embedded
/// `flasher_args.json`, bytes from the matching `EMBEDDED_*` blob). `set`
/// names the image set (`badge`, `relay`, `sync`) for display paths.
fn embedded_parts_for(
    set: &str,
    args_json: &str,
    bootloader: &[u8],
    part_table: &[u8],
    app: &[u8],
) -> Result<Vec<Part>> {
    let v: serde_json::Value =
        serde_json::from_str(args_json).context("parsing embedded flasher_args.json")?;
    let files = v
        .get("flash_files")
        .and_then(|f| f.as_object())
        .ok_or_else(|| anyhow::anyhow!("embedded flasher_args.json has no flash_files map"))?;
    let mut parts = Vec::new();
    for (addr_s, path_v) in files {
        let rel = path_v
            .as_str()
            .ok_or_else(|| anyhow::anyhow!("non-string path for offset {addr_s}"))?;
        // Match by file name: the embedded json uses image-set-relative
        // paths (`bootloader/bootloader.bin`, ...); anything else is the app.
        let data: &[u8] = if rel.ends_with("bootloader.bin") {
            bootloader
        } else if rel.ends_with("partition-table.bin") {
            part_table
        } else if rel.ends_with(".bin") {
            app
        } else {
            bail!("embedded firmware has no bytes for '{rel}' (extend EMBEDDED_* above)");
        };
        if data.is_empty() {
            bail!("embedded firmware file {rel} is empty");
        }
        let label = if set == "badge" {
            format!("<embedded>/{rel}")
        } else {
            format!("<embedded>/{set}/{rel}")
        };
        parts.push(Part {
            addr: parse_u32(addr_s)?,
            path: PathBuf::from(label),
            data: data.to_vec(),
        });
    }
    parts.sort_by_key(|p| p.addr);
    Ok(parts)
}

/// Default image set: the badge firmware.
fn embedded_parts() -> Result<Vec<Part>> {
    embedded_parts_for(
        "badge",
        EMBEDDED_FLASHER_ARGS,
        EMBEDDED_BOOTLOADER,
        EMBEDDED_PART_TABLE,
        EMBEDDED_APP,
    )
}

/// Relay image set (`--relay`).
fn embedded_relay_parts() -> Result<Vec<Part>> {
    embedded_parts_for(
        "relay",
        EMBEDDED_RELAY_ARGS,
        EMBEDDED_RELAY_BOOTLOADER,
        EMBEDDED_RELAY_PART_TABLE,
        EMBEDDED_RELAY_APP,
    )
}

/// Sync-station image set (`--sync`).
fn embedded_sync_parts() -> Result<Vec<Part>> {
    embedded_parts_for(
        "sync",
        EMBEDDED_SYNC_ARGS,
        EMBEDDED_SYNC_BOOTLOADER,
        EMBEDDED_SYNC_PART_TABLE,
        EMBEDDED_SYNC_APP,
    )
}

/// Deterministic SHA-256 over the image set: for each part in address order,
/// the 4-byte little-endian load address followed by its raw bytes. Same
/// bytes at the same offsets on any machine give the same hash.
fn firmware_hash(parts: &[Part]) -> [u8; 32] {
    let mut sorted: Vec<&Part> = parts.iter().collect();
    sorted.sort_by_key(|p| p.addr);
    let mut h = Sha256::new();
    for p in sorted {
        h.update(p.addr.to_le_bytes());
        h.update(&p.data);
    }
    h.finalize().into()
}

fn hex_hash(hash: &[u8; 32]) -> String {
    let mut s = String::with_capacity(64);
    for b in hash {
        use std::fmt::Write as _;
        let _ = write!(s, "{b:02x}");
    }
    s
}

/// GitHub-style identicon grid: 5x5, horizontally mirrored. Cell color comes
/// from the first 3 hash bytes; the next 15 bits fill the left 3 columns
/// (mirrored to the right), so the same image set always draws the same
/// mosaic on every machine.
fn identicon_grid(hash: &[u8; 32]) -> [[bool; 5]; 5] {
    let mut grid = [[false; 5]; 5];
    for row in 0..5 {
        for col in 0..3 {
            let i = row * 3 + col;
            let filled = (hash[3 + i / 8] >> (i % 8)) & 1 == 1;
            grid[row][col] = filled;
            grid[row][4 - col] = filled;
        }
    }
    grid
}

fn print_mosaic(hash: &[u8; 32]) {
    let grid = identicon_grid(hash);
    if std::env::var_os("NO_COLOR").is_some() {
        for row in &grid {
            let mut line = String::new();
            for &cell in row {
                line.push_str(if cell { "██" } else { "··" });
            }
            println!("  {line}");
        }
        return;
    }
    let (r, g, b) = (hash[0], hash[1], hash[2]);
    for row in &grid {
        let mut line = String::new();
        for &cell in row {
            if cell {
                line.push_str(&format!("\x1b[48;2;{r};{g};{b}m  \x1b[0m"));
            } else {
                line.push_str("\x1b[48;2;235;235;235m  \x1b[0m");
            }
        }
        println!("  {line}");
    }
}

fn load_parts(args: &Args) -> Result<Vec<Part>> {
    if args.relay {
        return embedded_relay_parts();
    }
    if args.sync {
        return embedded_sync_parts();
    }
    let mut specs: Vec<(u32, PathBuf)> = Vec::new();
    if let Some(json) = &args.flasher_args {
        specs.extend(load_flasher_args(json)?);
    }
    for p in &args.parts {
        let (addr, path) = parse_part(p)?;
        specs.push((addr, path));
    }
    if specs.is_empty() {
        // No arguments: flash the image set baked into the binary.
        return embedded_parts();
    }
    let mut parts = Vec::new();
    for (addr, path) in specs {
        let data = fs::read(&path)
            .with_context(|| format!("reading firmware file {}", path.display()))?;
        if data.is_empty() {
            bail!("firmware file {} is empty", path.display());
        }
        log::info!("part 0x{addr:05x}: {} ({} bytes)", path.display(), data.len());
        parts.push(Part { addr, path, data });
    }
    Ok(parts)
}

/// Current USB serial ports, deduplicated: macOS exposes both
/// `/dev/cu.*` and `/dev/tty.*` for one device; flash it once via `cu.*`.
fn current_ports() -> Vec<PortView> {
    let mut out = Vec::new();
    let ports = match serialport::available_ports() {
        Ok(p) => p,
        Err(e) => {
            log::warn!("port scan failed: {e}");
            return out;
        }
    };
    for info in ports {
        if let SerialPortType::UsbPort(usb) = info.port_type {
            out.push(PortView {
                port_name: info.port_name,
                vid: usb.vid,
                pid: usb.pid,
                serial: usb.serial_number,
            });
        }
    }
    // Dedupe by identity: prefer cu.* over tty.* (macOS), else first sorted.
    let mut by_id: HashMap<String, PortView> = HashMap::new();
    for pv in out {
        let id = pv.identity();
        match by_id.get(&id) {
            Some(existing)
                if existing.port_name.contains("/tty.") && pv.port_name.contains("/cu.") =>
            {
                by_id.insert(id, pv);
            }
            Some(_) => {}
            None => {
                by_id.insert(id, pv);
            }
        }
    }
    let mut deduped: Vec<PortView> = by_id.into_values().collect();
    deduped.sort_by(|a, b| a.port_name.cmp(&b.port_name));
    deduped
}

fn port_allowed(pv: &PortView, allow: &HashSet<(u16, u16)>, allow_any_usb: bool) -> bool {
    allow_any_usb || allow.contains(&(pv.vid, pv.pid))
}

/// Throttled per-device progress reporter (devices flash concurrently, so
/// every line is tagged with the board id).
struct TaggedProgress {
    tag: String,
    total: usize,
    next_pct: usize,
    t0: Instant,
}

impl TaggedProgress {
    fn new(tag: String) -> Self {
        Self { tag, total: 0, next_pct: 0, t0: Instant::now() }
    }
}

impl ProgressCallbacks for TaggedProgress {
    // NOTE: espflash reports progress in compressed-data chunks, not bytes:
    // `init` receives the chunk count, `update` the chunks written so far.
    fn init(&mut self, addr: u32, total: usize) {
        self.total = total;
        self.next_pct = 10;
        println!("[{}] writing 0x{addr:05x} ({total} chunks)", self.tag);
    }
    fn update(&mut self, current: usize) {
        if self.total == 0 {
            return;
        }
        let pct = current * 100 / self.total;
        if pct >= self.next_pct {
            println!(
                "[{}] ... {:>3}% ({current}/{} chunks, {:.0}s)",
                self.tag,
                pct,
                self.total,
                self.t0.elapsed().as_secs_f64()
            );
            self.next_pct += 25;
        }
    }
    fn verifying(&mut self) {
        println!("[{}] verifying...", self.tag);
    }
    fn finish(&mut self, _skipped: bool) {}
}

/// Outcome of one flash worker, reported back to the main loop.
#[derive(Debug)]
struct FlashResult {
    identity: String,
    display: String,
    vid: u16,
    pid: u16,
    ok: bool,
    detail: String,
    seconds: f64,
    bytes: usize,
}

#[derive(Debug, Clone)]
struct FlashConfig {
    parts: Vec<Part>,
    baud: u32,
    require_esp32c3: bool,
    verify: bool,
    settle_ms: u64,
    retries: u32,
}

/// Flash one board. The ESP32-C3 gate is enforced by espflash itself
/// (`ChipMismatch`), before any erase or write.
fn flash_one(pv: PortView, cfg: &FlashConfig) -> FlashResult {
    let display = format!("{} ({})", pv.short_id(), pv.port_name);
    let t0 = Instant::now();
    let bytes: usize = cfg.parts.iter().map(|p| p.data.len()).sum();
    thread::sleep(Duration::from_millis(cfg.settle_ms));

    let attempts = cfg.retries + 1;
    for attempt in 1..=attempts {
        match try_flash(&pv, cfg) {
            Ok(()) => {
                return FlashResult {
                    identity: pv.identity(),
                    display,
                    vid: pv.vid,
                    pid: pv.pid,
                    ok: true,
                    detail: format!("flashed {bytes} bytes (verified)"),
                    seconds: t0.elapsed().as_secs_f64(),
                    bytes,
                };
            }
            Err(e) => {
                // ChipMismatch is a deliberate refusal, never retried.
                if is_refusal(&e) {
                    return FlashResult {
                        identity: pv.identity(),
                        display,
                        vid: pv.vid,
                        pid: pv.pid,
                        ok: false,
                        detail: format!("REFUSED: {e:#}"),
                        seconds: t0.elapsed().as_secs_f64(),
                        bytes,
                    };
                }
                if attempt < attempts {
                    log::warn!("[{display}] attempt {attempt}/{attempts} failed: {e:#}; retrying");
                    thread::sleep(Duration::from_millis(500));
                } else {
                    return FlashResult {
                        identity: pv.identity(),
                        display,
                        vid: pv.vid,
                        pid: pv.pid,
                        ok: false,
                        detail: format!("{e:#}"),
                        seconds: t0.elapsed().as_secs_f64(),
                        bytes,
                    };
                }
            }
        }
    }
    unreachable!()
}

fn is_refusal(e: &anyhow::Error) -> bool {
    // espflash reports a wrong-chip device as ChipMismatch. Match the typed
    // error through the anyhow context chain (not the rendered string), with
    // a message fallback in case wrapping ever changes.
    e.chain().any(|cause| {
        matches!(
            cause.downcast_ref::<espflash::Error>(),
            Some(espflash::Error::ChipMismatch(..))
        ) || format!("{cause}").contains("does not match the detected chip")
    })
}

fn try_flash(pv: &PortView, cfg: &FlashConfig) -> Result<()> {
    let serial = serialport::new(&pv.port_name, 115_200)
        .timeout(Duration::from_secs(3))
        .open_native()
        .with_context(|| format!("opening {}", pv.port_name))?;

    // serialport may report UsbPort at scan time but Unknown on reopen
    // races; mirror espflash's own fallback (zeroed info) in that case.
    let usb_info = match serialport::available_ports()
        .unwrap_or_default()
        .into_iter()
        .find(|p| p.port_name == pv.port_name)
        .map(|p| p.port_type)
    {
        Some(SerialPortType::UsbPort(info)) => UsbPortInfo {
            vid: info.vid,
            pid: info.pid,
            serial_number: info.serial_number,
            manufacturer: info.manufacturer,
            product: info.product,
        },
        _ => UsbPortInfo {
            vid: pv.vid,
            pid: pv.pid,
            serial_number: pv.serial.clone(),
            manufacturer: None,
            product: None,
        },
    };

    let connection = Connection::new(
        serial,
        usb_info,
        ResetAfterOperation::HardReset,
        ResetBeforeOperation::DefaultReset,
        cfg.baud,
    );
    let chip = if cfg.require_esp32c3 { Some(Chip::Esp32c3) } else { None };
    let mut flasher = Flasher::connect(
        connection,
        true, // use stub loader (fast, required for high baud)
        cfg.verify,
        false, // skip: always write (fresh boards may hold stale data)
        chip,
        Some(cfg.baud),
    )
    .with_context(|| "connect")?;

    if cfg.require_esp32c3 && flasher.chip() != Chip::Esp32c3 {
        bail!("REFUSED: detected {:?}, not ESP32-C3", flasher.chip());
    }
    log::info!("[{}] connected: {:?}", pv.short_id(), flasher.chip());

    let segments: Vec<Segment> = cfg
        .parts
        .iter()
        .map(|p| Segment::new(p.addr, &p.data))
        .collect();
    let mut progress = TaggedProgress::new(pv.short_id());
    flasher
        .write_bins_to_flash(&segments, &mut progress)
        .with_context(|| "flash")?;

    let chip = flasher.chip();
    flasher
        .connection()
        .reset_after(true, chip)
        .with_context(|| "reset")?;
    Ok(())
}

/// Shared watcher state: what is flashing, and what already passed while
/// still plugged in (prevents reflash loops after the post-flash reboot).
#[derive(Default)]
struct Session {
    in_flight: HashSet<String>,
    flashed_ok: HashSet<String>,
}

fn csv_now() -> String {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs().to_string())
        .unwrap_or_else(|_| "0".into())
}

fn append_csv(path: &Path, r: &FlashResult) {
    // Write a header if the file is new/empty.
    let need_header = fs::metadata(path).map(|m| m.len() == 0).unwrap_or(true);
    let mut line = String::new();
    if need_header {
        line.push_str("timestamp,result,device,port,vid_pid,seconds,bytes,detail\n");
    }
    let detail = r.detail.replace('"', "'").replace('\n', " ");
    line.push_str(&format!(
        "{},{},{},\"{}\",{:04x}:{:04x},{:.1},{},\"{}\"\n",
        csv_now(),
        if r.ok { "OK" } else { "FAIL" },
        r.identity,
        r.display,
        r.vid,
        r.pid,
        r.seconds,
        r.bytes,
        detail,
    ));
    // Append; a failed log write must never fail the flash itself.
    use std::io::Write as _;
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = f.write_all(line.as_bytes());
    }
}

fn main() -> Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    let args = Args::parse();

    let mut allow: HashSet<(u16, u16)> = HashSet::new();
    for s in &args.vid_pid {
        allow.insert(parse_vid_pid(s)?);
    }
    let from_embedded = args.parts.is_empty() && args.flasher_args.is_none();
    let parts = load_parts(&args)?;
    let total_bytes: usize = parts.iter().map(|p| p.data.len()).sum();
    let source = if args.relay {
        "embedded firmware (relay)"
    } else if args.sync {
        "embedded firmware (sync)"
    } else if from_embedded {
        "embedded firmware (badge)"
    } else {
        "command-line firmware"
    };
    println!(
        "rapid-flash: {} part(s), {} bytes total @ {} baud ({})",
        parts.len(),
        total_bytes,
        args.baud,
        source,
    );
    for p in &parts {
        println!("  0x{:05x}: {} ({} bytes)", p.addr, p.path.display(), p.data.len());
    }
    // Image identity: same bytes at the same offsets draw the same mosaic
    // on every machine — eyeball it before a production run.
    let hash = firmware_hash(&parts);
    println!("firmware sha256: {}", hex_hash(&hash));
    print_mosaic(&hash);
    if args.allow_any_usb {
        println!("filter: any USB serial (ESP32-C3 chip check still enforced)");
    } else {
        let mut list: Vec<String> = allow.iter().map(|(v, p)| format!("{v:04x}:{p:04x}")).collect();
        list.sort();
        println!("filter: USB VID:PID [{}] + ESP32-C3 chip check", list.join(", "));
    }

    if args.dry_run {
        println!("dry-run: reporting matching ports, flashing disabled");
    }

    let cfg = FlashConfig {
        parts,
        baud: args.baud,
        require_esp32c3: !args.allow_any_chip,
        verify: true, // unconditional: never ship an unverified board
        settle_ms: args.settle_ms,
        retries: args.retries,
    };

    let running = Arc::new(AtomicBool::new(true));
    {
        let running = running.clone();
        let _ = ctrlc::set_handler(move || {
            static SECOND: AtomicBool = AtomicBool::new(false);
            if SECOND.swap(true, Ordering::SeqCst) {
                // Second Ctrl-C: operator insists; unclean exit.
                std::process::exit(130);
            }
            eprintln!("\nshutting down (in-flight flashes are left to finish)...");
            running.store(false, Ordering::SeqCst);
        });
    }

    let session = Arc::new(Mutex::new(Session::default()));
    let (tx, rx) = std::sync::mpsc::channel::<FlashResult>();
    let mut counts = (0u32, 0u32); // (ok, fail)

    // Arrival detection: a port is new when it was absent on the previous
    // poll. With --no-initial-scan, seed with what's already here so
    // pre-existing boards are left alone until their next plug-in.
    let mut prev_present: HashSet<String> = HashSet::new();
    if args.no_initial_scan {
        prev_present = current_ports().iter().map(|p| p.identity()).collect();
    }

    while running.load(Ordering::SeqCst) {
        // Collect finished workers without blocking the scan loop.
        while let Ok(r) = rx.try_recv() {
            {
                let mut s = session.lock().unwrap();
                s.in_flight.remove(&r.identity);
                if r.ok {
                    s.flashed_ok.insert(r.identity.clone());
                }
            }
            if r.ok {
                counts.0 += 1;
                println!("[OK] {}: {} in {:.1}s", r.display, r.detail, r.seconds);
            } else {
                counts.1 += 1;
                println!("[FAIL] {}: {} ({:.1}s)", r.display, r.detail, r.seconds);
            }
            if let Some(csv) = &args.log_csv {
                append_csv(csv, &r);
            }
        }

        let ports = current_ports();
        let now_present: HashSet<String> = ports.iter().map(|p| p.identity()).collect();
        // Boards that vanished are eligible again on their next plug-in.
        // (In-flight entries stay until their worker reports; the worker
        // owns that cleanup via the results channel.)
        {
            let mut s = session.lock().unwrap();
            s.flashed_ok.retain(|id| now_present.contains(id));
        }

        for pv in &ports {
            if !port_allowed(pv, &allow, args.allow_any_usb) {
                continue;
            }
            let id = pv.identity();
            if prev_present.contains(&id) {
                continue; // already here last poll: not an arrival
            }
            let mut s = session.lock().unwrap();
            if s.in_flight.contains(&id) || s.flashed_ok.contains(&id) {
                continue;
            }
            if args.dry_run {
                println!(
                    "[DRY] would flash {} ({:04x}:{:04x} on {})",
                    pv.short_id(),
                    pv.vid,
                    pv.pid,
                    pv.port_name
                );
                s.flashed_ok.insert(id);
                continue;
            }
            s.in_flight.insert(id.clone());
            drop(s);
            let worker_pv = pv.clone();
            let worker_cfg = cfg.clone();
            let worker_tx = tx.clone();
            let worker_session = session.clone();
            match thread::Builder::new()
                .name(format!("flash-{}", pv.short_id()))
                .spawn(move || {
                    let r = flash_one(worker_pv, &worker_cfg);
                    let _ = worker_tx.send(r);
                })
            {
                Ok(_) => println!("[{}] detected on {}, flashing...", pv.short_id(), pv.port_name),
                Err(e) => {
                    log::error!("[{}] could not spawn worker: {e}", pv.short_id());
                    worker_session.lock().unwrap().in_flight.remove(&id);
                }
            }
        }
        prev_present = now_present;

        if args.once && session.lock().unwrap().in_flight.is_empty() {
            // Nothing matched, or every initial worker already reported
            // through the drain above.
            println!("done: {} ok, {} failed", counts.0, counts.1);
            if counts.1 > 0 {
                std::process::exit(1);
            }
            return Ok(());
        }

        thread::sleep(Duration::from_millis(args.poll_ms));
    }

    // Ctrl-C: stop watching, but let in-flight flashes land — killing the
    // process mid-write is exactly how boards get bricked.
    let pending = session.lock().unwrap().in_flight.len();
    if pending > 0 {
        println!("waiting for {pending} in-flight flash(es) — Ctrl-C again to abandon...");
    }
    while !session.lock().unwrap().in_flight.is_empty() {
        match rx.recv() {
            Ok(r) => {
                session.lock().unwrap().in_flight.remove(&r.identity);
                if r.ok {
                    counts.0 += 1;
                    println!("[OK] {}: {} in {:.1}s", r.display, r.detail, r.seconds);
                } else {
                    counts.1 += 1;
                    println!("[FAIL] {}: {} ({:.1}s)", r.display, r.detail, r.seconds);
                }
                if let Some(csv) = &args.log_csv {
                    append_csv(csv, &r);
                }
            }
            Err(_) => break, // all senders gone; nothing left to wait for
        }
    }
    println!("stopped: {} ok, {} failed", counts.0, counts.1);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pv(name: &str, vid: u16, pid: u16, serial: Option<&str>) -> PortView {
        PortView {
            port_name: name.into(),
            vid,
            pid,
            serial: serial.map(|s| s.into()),
        }
    }

    #[test]
    fn addrs_parse_hex_and_dec() {
        assert_eq!(parse_u32("0x10000").unwrap(), 0x10000);
        assert_eq!(parse_u32("0X0").unwrap(), 0);
        assert_eq!(parse_u32("65536").unwrap(), 65536);
        assert!(parse_u32("xyz").is_err());
    }

    #[test]
    fn parts_parse_offset_path() {
        let (addr, path) = parse_part("0x10000:app.bin").unwrap();
        assert_eq!((addr, path), (0x10000, PathBuf::from("app.bin")));
        assert!(parse_part("nocolon").is_err());
        assert!(parse_part("0x0:").is_err());
    }

    #[test]
    fn vid_pid_parse() {
        assert_eq!(parse_vid_pid("303a:1001").unwrap(), (0x303a, 0x1001));
        assert_eq!(parse_vid_pid("0x303A:0x1001").unwrap(), (0x303a, 0x1001));
        assert!(parse_vid_pid("303a").is_err());
        assert!(parse_vid_pid("zz:11").is_err());
    }

    #[test]
    fn identity_prefers_usb_serial() {
        // USB serial (== MAC on ESP parts) is stable across re-enumeration.
        let a = pv("/dev/cu.usbmodem1101", 0x303a, 0x1001, Some("E8:3D:C1:27:C4:88"));
        let b = pv("/dev/tty.usbmodem1101", 0x303a, 0x1001, Some("E8:3D:C1:27:C4:88"));
        assert_eq!(a.identity(), b.identity());
        let c = pv("/dev/cu.usbmodem1101", 0x303a, 0x1001, None);
        assert!(c.identity().starts_with("port:"));
    }

    #[test]
    fn filter_allows_only_listed_vid_pid() {
        let allow: HashSet<(u16, u16)> = [parse_vid_pid(DEFAULT_VID_PID).unwrap()].into();
        assert!(port_allowed(&pv("p", 0x303a, 0x1001, None), &allow, false));
        assert!(!port_allowed(&pv("p", 0x10c4, 0xea60, None), &allow, false));
        assert!(port_allowed(&pv("p", 0x10c4, 0xea60, None), &allow, true));
    }

    #[test]
    fn flasher_args_json_loads_offsets_relative_to_json() {
        let dir = std::env::temp_dir().join("rapid-flash-test");
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        fs::write(dir.join("boot.bin"), [0u8; 16]).unwrap();
        fs::write(dir.join("app.bin"), [1u8; 32]).unwrap();
        fs::write(
            dir.join("flasher_args.json"),
            r#"{"flash_files": {"0x10000": "app.bin", "0x0": "boot.bin"}}"#,
        )
        .unwrap();
        let parts = load_flasher_args(&dir.join("flasher_args.json")).unwrap();
        assert_eq!(parts.len(), 2);
        assert_eq!(parts[0].0, 0x0); // sorted by address
        assert_eq!(parts[1].0, 0x10000);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn chip_mismatch_counts_as_refusal() {
        let e = Err::<(), _>(espflash::Error::ChipMismatch("esp32c3".into(), "esp32s3".into()))
            .with_context(|| "connect")
            .unwrap_err();
        assert!(is_refusal(&e));
        assert!(!is_refusal(&anyhow::anyhow!("opening /dev/x")));
    }

    fn test_part(addr: u32, data: &[u8]) -> Part {
        Part {
            addr,
            path: PathBuf::from("test.bin"),
            data: data.to_vec(),
        }
    }

    #[test]
    fn embedded_parts_match_checked_in_firmware_dir() {
        let args = Args::try_parse_from(["rapid-flash"]).unwrap();
        assert!(args.parts.is_empty() && args.flasher_args.is_none());
        let parts = load_parts(&args).unwrap();
        assert_eq!(parts.len(), 3);
        let addrs: Vec<u32> = parts.iter().map(|p| p.addr).collect();
        assert_eq!(addrs, vec![0x0, 0x8000, 0x10000]);
        assert!(parts.iter().all(|p| !p.data.is_empty()));
        assert!(parts
            .iter()
            .all(|p| p.path.to_string_lossy().starts_with("<embedded>/")));
    }

    #[test]
    fn relay_flag_selects_relay_parts() {
        let args = Args::try_parse_from(["rapid-flash", "--relay"]).unwrap();
        assert!(args.relay && !args.sync);
        let parts = load_parts(&args).unwrap();
        assert_eq!(parts.len(), 3);
        let addrs: Vec<u32> = parts.iter().map(|p| p.addr).collect();
        assert_eq!(addrs, vec![0x0, 0x8000, 0x10000]);
        assert!(parts.iter().all(|p| !p.data.is_empty()));
        assert!(parts
            .iter()
            .all(|p| p.path.to_string_lossy().starts_with("<embedded>/relay/")));
        assert!(parts
            .iter()
            .any(|p| p.path.to_string_lossy().ends_with("relay_firmware.bin")));
    }

    #[test]
    fn sync_flag_selects_sync_parts() {
        let args = Args::try_parse_from(["rapid-flash", "--sync"]).unwrap();
        assert!(args.sync && !args.relay);
        let parts = load_parts(&args).unwrap();
        assert_eq!(parts.len(), 3);
        let addrs: Vec<u32> = parts.iter().map(|p| p.addr).collect();
        assert_eq!(addrs, vec![0x0, 0x8000, 0x10000]);
        assert!(parts.iter().all(|p| !p.data.is_empty()));
        assert!(parts
            .iter()
            .all(|p| p.path.to_string_lossy().starts_with("<embedded>/sync/")));
        assert!(parts
            .iter()
            .any(|p| p.path.to_string_lossy().ends_with("sync_station.bin")));
    }

    #[test]
    fn firmware_set_flags_conflict() {
        assert!(Args::try_parse_from(["rapid-flash", "--relay", "--sync"]).is_err());
        assert!(Args::try_parse_from(["rapid-flash", "--relay", "--part", "0x0:x.bin"]).is_err());
        assert!(Args::try_parse_from(["rapid-flash", "--sync", "--flasher-args", "x.json"])
            .is_err());
    }

    #[test]
    fn firmware_hash_is_stable_and_order_insensitive() {
        let a = vec![test_part(0x0, b"boot"), test_part(0x10000, b"app")];
        let mut b = a.clone();
        b.reverse();
        assert_eq!(firmware_hash(&a), firmware_hash(&b));
        assert_eq!(hex_hash(&firmware_hash(&a)).len(), 64);
    }

    #[test]
    fn firmware_hash_moves_with_bytes_or_address() {
        let base = vec![test_part(0x0, b"boot"), test_part(0x10000, b"app")];
        let h0 = firmware_hash(&base);
        let mut changed_byte = base.clone();
        changed_byte[1].data[0] ^= 0xff;
        assert_ne!(firmware_hash(&changed_byte), h0);
        let mut changed_addr = base.clone();
        changed_addr[1].addr = 0x20000;
        assert_ne!(firmware_hash(&changed_addr), h0);
    }

    #[test]
    fn identicon_is_symmetric_and_deterministic() {
        let hash = firmware_hash(&[test_part(0x0, b"boot")]);
        let g1 = identicon_grid(&hash);
        let g2 = identicon_grid(&hash);
        assert_eq!(g1, g2);
        for row in &g1 {
            assert_eq!(row[0], row[4]);
            assert_eq!(row[1], row[3]);
        }
        // A different image set draws a different mosaic.
        let other = identicon_grid(&firmware_hash(&[test_part(0x0, b"other")]));
        assert_ne!(g1, other);
    }
}

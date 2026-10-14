#!/usr/bin/env python3
"""Badge as screen + WASD controller (HTN OS proxy firmware).

One script, stdlib only, no pip. Talks to the badge service from the API
doc (production https://badge.solana-htn.com, local http://localhost:3100):

  mirror  PC screen  --->  badge screen  (POST /v1/badges/:id/image)
  keys    badge buttons --> PC WASD keys  (GET  /v1/badges/:id/events + SSE)
  play    demo maze game mirrored to the badge, controlled by badge or PC WASD

Button map (press edge only, releases ignored):
  Up->w  Left->a  Down->s  Right->d   (the WASD core)
  A->space  B->escape  Start->enter   (extras, disable with --wasd-only)

Usage:
  export BADGE_ID=xb2b9 BADGE_KEY=hunter2
  tools/wasd-mirror/wasd_mirror.py mirror --fps 1
  tools/wasd-mirror/wasd_mirror.py keys [--wasd-only] [--no-inject]
  tools/wasd-mirror/wasd_mirror.py play [--wasd-only]

Key injection needs no dependencies: macOS uses osascript/System Events,
Linux tries xdotool when present. Without either it still prints the keys,
so any game reading stdin works. Accessibility permission may be required
for osascript keystrokes to reach other apps.
"""

import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import zlib
import urllib.error
import urllib.parse
import urllib.request

BASE_DEFAULT = "https://badge.solana-htn.com"

# D-pad is the WASD core; the rest are convenience extras.
WASD = {"up": "w", "left": "a", "down": "s", "right": "d"}
EXTRAS = {"a": "space", "b": "escape", "start": "enter"}
BTN_MAP = dict(WASD)
BTN_MAP.update(EXTRAS)


def api_base(args):
    return (args.base or os.environ.get("BADGE_BASE", BASE_DEFAULT)).rstrip("/")


def api_id(args):
    bid = args.badge or os.environ.get("BADGE_ID")
    if not bid:
        sys.exit("missing badge id: --badge or $BADGE_ID")
    return bid


def api_key(args):
    key = args.key or os.environ.get("BADGE_KEY")
    if not key:
        sys.exit("missing app key: --key or $BADGE_KEY")
    return key


def rest(method, url, key, body=None, ctype="application/json", timeout=8):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"X-Badge-Key": key,
                                          "Content-Type": ctype})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
            return json.loads(raw) if raw else {}
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:200]
        sys.exit(f"HTTP {e.code} {url}: {detail}")


def post_json(args, path, body):
    return rest("POST", f"{api_base(args)}/v1/badges/{api_id(args)}/{path}",
                api_key(args), body)


def check_badge(args):
    url = f"{api_base(args)}/v1/badges/{api_id(args)}"
    try:
        with urllib.request.urlopen(url, timeout=8) as r:
            st = json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        sys.exit(f"HTTP {e.code}: unknown badge '{api_id(args)}'?")
    if not st.get("online"):
        sys.exit(f"badge {api_id(args)} is offline (registered, not connected).")
    if not st.get("hasKey"):
        sys.exit("badge has no app key yet (Settings -> App key on the badge).")
    return st


# ---------------------------------------------------------------- mirror ---

def capture_screen(tmp_jpg, width=320):
    """Capture the desktop to a small JPEG. macOS first, Linux fallback."""
    if shutil.which("screencapture"):
        subprocess.run(["screencapture", "-x", "-t", "jpg", tmp_jpg],
                       check=True, timeout=15)
        if shutil.which("sips"):  # keep under the 512 KiB upload cap
            subprocess.run(["sips", "-Z", str(width), tmp_jpg],
                           check=True, capture_output=True, timeout=15)
        return "image/jpeg"
    for cmd in (["grim", tmp_jpg], ["import", "-window", "root", tmp_jpg],
                ["scrot", "--silent", tmp_jpg]):
        if shutil.which(cmd[0]):
            subprocess.run(cmd, check=True, timeout=15)
            return "image/jpeg"
    sys.exit("no screen capture tool found (macOS: screencapture, "
             "Linux: grim/import/scrot).")


# ---------------- minimal PNG codec (stdlib) for the tiled fast path -------

def decode_png_rgb(path):
    """Decode an 8-bit non-interlaced PNG (color types 2/6) to (w, h, rgb).

    Only what sips/grim/scrot emit. Returns rows as flat RGB bytes.
    """
    import struct
    data = open(path, "rb").read()
    assert data[:8] == bytes([137, 80, 78, 71, 13, 10, 26, 10]), "not a PNG"
    off, raw, w = 8, b"", None
    while off < len(data):
        (ln, typ) = struct.unpack(">I4s", data[off:off + 8])
        body = data[off + 8:off + 8 + ln]
        if typ == b"IHDR":
            w, h, bd, ct, _, _, il = struct.unpack(">IIBBBBB", body)
            assert bd == 8 and ct in (2, 6) and il == 0, \
                f"unsupported PNG (depth={bd} type={ct} ilace={il})"
            ch = 3 if ct == 2 else 4
        elif typ == b"IDAT":
            raw += body
        elif typ == b"IEND":
            break
        off += 12 + ln
    px = zlib.decompress(raw)
    rgb = bytearray(w * h * 3)
    prev = bytearray(w * ch)
    pos = 0
    for y in range(h):
        f = px[pos]
        pos += 1
        line = bytearray(px[pos:pos + w * ch])
        pos += w * ch
        if f == 1:  # Sub
            for i in range(ch, len(line)):
                line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:  # Up
            for i in range(len(line)):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:  # Average
            for i in range(len(line)):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:  # Paeth
            for i in range(len(line)):
                a = line[i - ch] if i >= ch else 0
                b, c = prev[i], prev[i - ch] if i >= ch else 0
                p, pa, pb, pc = a + b - c, 0, 0, 0
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        elif f != 0:
            raise ValueError(f"bad PNG filter {f}")
        prev = line
        if ch == 3:
            rgb[y * w * 3:(y + 1) * w * 3] = line
        else:  # RGBA -> RGB: drop alpha
            rgb[y * w * 3:(y + 1) * w * 3] = b"".join(
                bytes(line[i:i + 3]) for i in range(0, len(line), 4))
    return w, h, bytes(rgb)


def encode_png_rgb(w, h, rgb):
    """Encode flat RGB bytes to a minimal PNG (filter 0, one IDAT)."""
    import struct
    assert len(rgb) == w * h * 3
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3]
                    for y in range(h))
    comp = zlib.compress(raw, 6)

    def chunk(typ, body):
        return (struct.pack(">I", len(body)) + typ + body
                + struct.pack(">I", zlib.crc32(typ + body) & 0xffffffff))
    return (bytes([137, 80, 78, 71, 13, 10, 26, 10])
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", comp) + chunk(b"IEND", b""))


def capture_small_png(tmp_png, width=320):
    """macOS: screencapture to PNG, sips down so the long side is width."""
    subprocess.run(["screencapture", "-x", "-t", "png", tmp_png],
                   check=True, timeout=15)
    subprocess.run(["sips", "-Z", str(width), tmp_png],
                   check=True, capture_output=True, timeout=15)


class Bucket:
    """Token bucket guarding the service rate limit (20 cmds/s, burst 40)."""

    def __init__(self, rate=14.0, burst=30.0):
        self.rate, self.burst = rate, burst
        self.tokens, self.stamp = burst, time.monotonic()
        self.lock = threading.Lock()

    def take(self, n=1):
        while True:
            with self.lock:
                now = time.monotonic()
                self.tokens = min(self.burst,
                                  self.tokens + (now - self.stamp) * self.rate)
                self.stamp = now
                if self.tokens >= n:
                    self.tokens -= n
                    return
                wait = (n - self.tokens) / self.rate
            time.sleep(wait)


def post_image_bytes(args, data, ctype, x=None, y=None, fit="contain"):
    q = f"?x={x}&y={y}&fit={fit}" if x is not None else f"?fit={fit}"
    url = f"{api_base(args)}/v1/badges/{api_id(args)}/image{q}"
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"X-Badge-Key": api_key(args),
                                          "Content-Type": ctype})
    with urllib.request.urlopen(req, timeout=12) as r:
        return json.loads(r.read().decode() or "{}")


TILE_W, TILE_H = 64, 40
TILE_WORKERS = 6


def cmd_mirror_fast(args):
    """Tiled-diff mirror: full keyframe, then only changed tiles.

    A full-screen blit takes ~1 s on the wire, so full frames cap out at
    ~1 fps. Tiles (64x40 px, a few KB) upload in parallel, so a moving
    cursor or game sprite refreshes in a fraction of that.
    """
    from concurrent.futures import ThreadPoolExecutor
    check_badge(args)
    bucket = Bucket()
    print(f"fast mirror screen -> badge {api_id(args)} "
          f"(tiles {TILE_W}x{TILE_H}, Ctrl-C to stop).")
    with tempfile.TemporaryDirectory(prefix="wasd-mirror-") as td:
        shot = os.path.join(td, "shot.png")

        def keyframe(rgb, w, h):
            bucket.take()
            post_image_bytes(args, encode_png_rgb(w, h, rgb),
                             "image/png", fit="contain")

        def tile_changed(tile, prev):
            return tile != prev

        capture_small_png(shot)
        w, h, rgb = decode_png_rgb(shot)
        keyframe(rgb, w, h)
        ox, oy = (320 - w) // 2, (240 - h) // 2
        cols, rows = (w + TILE_W - 1) // TILE_W, (h + TILE_H - 1) // TILE_H
        prev = {}

        def grab_tile(tx, ty):
            x0, y0 = tx * TILE_W, ty * TILE_H
            tw, th = min(TILE_W, w - x0), min(TILE_H, h - y0)
            out = bytearray(tw * th * 3)
            for r in range(th):
                s = ((y0 + r) * w + x0) * 3
                out[r * tw * 3:(r + 1) * tw * 3] = rgb[s:s + tw * 3]
            return tw, th, bytes(out)

        for ty in range(rows):
            for tx in range(cols):
                prev[(tx, ty)] = grab_tile(tx, ty)[2]
        last_key, n, t0 = time.time(), 0, time.time()

        pool = ThreadPoolExecutor(max_workers=TILE_WORKERS)
        try:
            while True:
                t0c = time.time()
                try:
                    capture_small_png(shot)
                    w2, h2, rgb = decode_png_rgb(shot)
                except Exception as e:
                    print(f"warn: capture failed ({e}), retrying",
                          file=sys.stderr)
                    time.sleep(0.5)
                    continue
                if (w2, h2) != (w, h):  # resolution changed: start over
                    w, h = w2, h2
                    keyframe(rgb, w, h)
                    ox, oy = (320 - w) // 2, (240 - h) // 2
                    cols = (w + TILE_W - 1) // TILE_W
                    rows = (h + TILE_H - 1) // TILE_H
                    prev = {(tx, ty): grab_tile(tx, ty)[2]
                            for ty in range(rows) for tx in range(cols)}
                    last_key = time.time()
                    continue
                changed = []
                for ty in range(rows):
                    for tx in range(cols):
                        tw, th, tile = grab_tile(tx, ty)
                        if tile_changed(tile, prev.get((tx, ty))):
                            changed.append((tx, ty, tw, th, tile))
                if len(changed) > 10 or time.time() - last_key > 8:
                    keyframe(rgb, w, h)  # cheaper than 10+ tiles; heals misses
                    for tx, ty, _, _, tile in changed:
                        prev[(tx, ty)] = tile
                    last_key = time.time()
                    n += 1
                elif changed:
                    def up(t):
                        tx, ty, tw, th, tile = t
                        bucket.take()
                        try:
                            post_image_bytes(
                                args, encode_png_rgb(tw, th, tile),
                                "image/png", x=ox + tx * TILE_W,
                                y=oy + ty * TILE_H, fit="none")
                            prev[(tx, ty)] = tile
                        except Exception as e:
                            print(f"warn: tile -> {e}", file=sys.stderr)
                    list(pool.map(up, changed))
                    n += 1
                el = time.time() - t0c
                fps = n / max(0.1, time.time() - t0)
                print(f"\r{n} updates ({fps:.1f}/s) last: "
                      f"{len(changed)} tiles in {el * 1000:.0f} ms   ",
                      end="", flush=True)
        finally:
            pool.shutdown(wait=False)


def cmd_mirror(args):
    check_badge(args)
    period = 1.0 / max(0.2, min(args.fps, 5.0))
    print(f"mirroring screen -> badge {api_id(args)} "
          f"at {1.0 / period:.1f} fps (Ctrl-C to stop).")
    with tempfile.TemporaryDirectory(prefix="wasd-mirror-") as td:
        shot = os.path.join(td, "shot.jpg")
        n = 0
        while True:
            t0 = time.time()
            ctype = capture_screen(shot)
            size = os.path.getsize(shot)
            if size > 512 * 1024:
                sys.exit(f"capture is {size} bytes, over the 512 KiB cap; "
                         "lower your resolution and retry.")
            with open(shot, "rb") as f:
                pixels = f.read()
            url = (f"{api_base(args)}/v1/badges/{api_id(args)}/image"
                   "?fit=contain")
            req = urllib.request.Request(url, data=pixels, method="POST",
                                         headers={"X-Badge-Key": api_key(args),
                                                  "Content-Type": ctype})
            try:
                with urllib.request.urlopen(req, timeout=15) as r:
                    json.loads(r.read().decode() or "{}")
            except urllib.error.HTTPError as e:
                print(f"warn: image POST -> HTTP {e.code}, retrying",
                      file=sys.stderr)
            n += 1
            print(f"\rframe {n} ({size // 1024} KB)", end="", flush=True)
            time.sleep(max(0.0, period - (time.time() - t0)))


# ---------------------------------------------------------------- keys -----

def inject_keystroke(ch, no_inject):
    """Deliver one key to the focused app. Returns how it was delivered."""
    if no_inject:
        return "print"
    if sys.platform == "darwin" and shutil.which("osascript"):
        key = {"space": " ", "escape": "⎋", "enter": "\r"}.get(ch, ch)
        if ch == "escape":
            script = ('tell application "System Events" to key code 53')
        else:
            script = (f'tell application "System Events" to keystroke "{key}"')
        r = subprocess.run(["osascript", "-e", script], capture_output=True,
                           timeout=5)
        return "osascript" if r.returncode == 0 else "print-fallback"
    if shutil.which("xdotool"):
        r = subprocess.run(["xdotool", "key", ch], capture_output=True,
                           timeout=5)
        return "xdotool" if r.returncode == 0 else "print-fallback"
    return "print"


def sse_buttons(args, out_q, stop):
    """GET /events SSE loop; pushes (button, pressed) for press edges."""
    url = (f"{api_base(args)}/v1/badges/{api_id(args)}/events"
           f"?key={urllib.parse.quote(api_key(args))}")
    req = urllib.request.Request(url, headers={"Accept": "text/event-stream"})
    backoff = 2.0
    while not stop.is_set():
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                backoff = 2.0
                buf = b""
                while not stop.is_set():
                    chunk = r.read(1)
                    if not chunk:
                        break
                    buf += chunk
                    if not buf.endswith(b"\n\n"):
                        continue
                    block, buf = buf.decode("utf-8", "replace"), b""
                    ev_name, payload = None, None
                    for line in block.splitlines():
                        if line.startswith("event:"):
                            ev_name = line[6:].strip()
                        elif line.startswith("data:"):
                            payload = line[5:].strip()
                    if ev_name in ("button", None) and payload:
                        try:
                            ev = json.loads(payload)
                        except json.JSONDecodeError:
                            continue
                        if (ev.get("event") == "button"
                                and ev.get("pressed")
                                and ev.get("button") in BTN_MAP):
                            out_q.put(ev["button"])
                    elif payload and '"event": "offline"' in payload:
                        print("badge went offline.", file=sys.stderr)
        except Exception as e:  # reconnect with back-off, like the badge link
            if stop.is_set():
                break
            print(f"sse drop ({e}); retry in {backoff:.0f}s", file=sys.stderr)
            stop.wait(backoff)
            backoff = min(30.0, backoff * 2)


def cmd_keys(args):
    check_badge(args)
    if args.wasd_only:
        for b in list(BTN_MAP):
            if b not in WASD:
                del BTN_MAP[b]
    mode = "WASD only" if args.wasd_only else "WASD + space/esc/enter"
    print(f"badge {api_id(args)} buttons -> PC keys ({mode}). Ctrl-C to stop.")
    print("mapping:", " ".join(f"{b}={k}" for b, k in BTN_MAP.items()))
    out_q: queue.Queue = queue.Queue()
    stop = threading.Event()
    threading.Thread(target=sse_buttons, args=(args, out_q, stop),
                     daemon=True).start()
    try:
        while True:
            btn = out_q.get()
            ch = BTN_MAP[btn]
            how = inject_keystroke(ch, args.no_inject)
            print(f"{btn:6s} -> {ch} [{how}]", flush=True)
    except KeyboardInterrupt:
        stop.set()


# ---------------------------------------------------------------- play -----

MAZE = [
    "####################",
    "#S....#...........##",
    "#####.#.#####.###..#",
    "#.....#.......#.#.E#",
    "#.#########.###.#.##",
    "#.#...........#.#..#",
    "#.#.#####.###.#.##.#",
    "#.#.#...#.#...#....#",
    "#.#.#.#.#.#.#####.##",
    "#...#.#...#.....#..#",
    "#####.#######.#.#.##",
    "#.....#.....#.#.#..#",
    "#.###.#.###.#.#.##.#",
    "#...#...#...#......#",
    "####################",
]
PW, PH = 20, 15  # maze cells; must match MAZE above


def render_ascii(px, py, moves, t0):
    el = time.time() - t0
    print("\033[H\033[J", end="")
    print(f"WASD maze  (badge mirrors this)  moves={moves}  {el:.0f}s")
    print("WASD on PC stdin, or badge D-pad. Reach E. q quits.")
    for y, row in enumerate(MAZE):
        line = ""
        for x, c in enumerate(row):
            line += "@" if (x, y) == (px, py) else c
        print(line)


def maze_text(px, py):
    rows = []
    for y, row in enumerate(MAZE):
        rows.append("".join("@" if (x, y) == (px, py) else c
                            for x, c in enumerate(row)))
    return "\n".join(rows)


def draw_badge(args, px, py, moves, t0, msg=""):
    """Mirror the maze to the badge: two text commands per redraw.

    Size 1 is 6x12 px per glyph: 20 cols = 120 px, and head + 15 rows =
    192 px, so it fits 320x240. The full maze is ~315 chars but text caps
    at 256, so it goes as two posts (second without clear). Two commands
    per move stays far under the 20 commands/s budget.
    """
    el = time.time() - t0
    head = f"{moves}mv {el:.0f}s {msg}".strip()[:26]
    rows = maze_text(px, py).split("\n")
    post_json(args, "text", {
        "text": head + "\n" + "\n".join(rows[:8]),
        "x": 0, "y": 0, "size": 1, "color": "#ffffff",
        "background": "#000000", "clear": True})
    post_json(args, "text", {
        "text": "\n".join(rows[8:]),
        "x": 0, "y": 9 * 12, "size": 1, "color": "#ffffff",
        "background": "#000000"})


def step(px, py, dx, dy):
    nx, ny = px + dx, py + dy
    if 0 <= ny < PH and 0 <= nx < PW and MAZE[ny][nx] != "#":
        return nx, ny, True
    return px, py, False


DIRS = {"w": (0, -1), "a": (-1, 0), "s": (0, 1), "d": (1, 0),
        "up": (0, -1), "left": (-1, 0), "down": (0, 1), "right": (1, 0)}


def cmd_play(args):
    check_badge(args)
    if args.wasd_only:
        for b in list(BTN_MAP):
            if b not in WASD:
                del BTN_MAP[b]
    try:
        px, py = next(
            (x, y) for y, row in enumerate(MAZE)
            for x, c in enumerate(row) if c == "S")
    except StopIteration:
        sys.exit("maze has no start cell.")
    moves, t0 = 0, time.time()
    btn_q: queue.Queue = queue.Queue()
    stop = threading.Event()

    def pump():
        eq: queue.Queue = queue.Queue()
        threading.Thread(target=sse_buttons, args=(args, eq, stop),
                         daemon=True).start()
        while not stop.is_set():
            try:
                btn_q.put(eq.get(timeout=0.2))
            except queue.Empty:
                pass

    threading.Thread(target=pump, daemon=True).start()
    post_json(args, "text", {"text": "WASD MAZE", "x": 90, "y": 100,
                             "size": 3, "color": "#9945ff", "clear": True})
    time.sleep(0.8)
    render_ascii(px, py, moves, t0)
    draw_badge(args, px, py, moves, t0)
    print("badge mirrored. Type WASD + enter, or press the badge D-pad.")

    import select
    try:
        render_ascii(px, py, moves, t0)
        while True:
            moved = False
            while not btn_q.empty():
                btn = btn_q.get()
                key = BTN_MAP.get(btn, btn)
                dx, dy = DIRS.get(key, (0, 0))
                px, py, ok = step(px, py, dx, dy)
                moves += ok
                moved = moved or ok
            if select.select([sys.stdin], [], [], 0.1)[0]:
                ch = sys.stdin.readline().strip().lower()
                if ch in ("q", "quit"):
                    break
                for c in ch:
                    if c in DIRS:
                        dx, dy = DIRS[c]
                        px, py, ok = step(px, py, dx, dy)
                        moves += ok
                        moved = moved or ok
            if moved:
                render_ascii(px, py, moves, t0)
                draw_badge(args, px, py, moves, t0)
            if MAZE[py][px] == "E":
                el = time.time() - t0
                post_json(args, "leds", {"all": "#00ff00"})
                post_json(args, "text", {
                    "text": f"WIN {moves} moves {el:.0f}s", "x": 40, "y": 110,
                    "size": 2, "color": "#14f195", "clear": True})
                print(f"\nYou win! {moves} moves, {el:.0f}s.")
                break
            time.sleep(0.15)  # ~6 draw/s x ~30 small cmds stays in budget
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        post_json(args, "leds", {"all": "#000000"})
        post_json(args, "home", {})


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", default=None, help="service base URL")
    ap.add_argument("--badge", default=None, help="5-char HTN-ID")
    ap.add_argument("--key", default=None, help="badge app key")
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("mirror", help="push the PC screen to the badge")
    m.add_argument("--fps", type=float, default=1.0)
    m.add_argument("--full", action="store_true",
                   help="full-frame JPEG path instead of fast tiled diffs")
    k = sub.add_parser("keys", help="badge buttons type WASD on this PC")
    k.add_argument("--wasd-only", action="store_true")
    k.add_argument("--no-inject", action="store_true",
                   help="print keys instead of typing them")
    p = sub.add_parser("play", help="demo WASD maze mirrored to the badge")
    p.add_argument("--wasd-only", action="store_true")
    args = ap.parse_args()
    if (args.cmd == "mirror" and not args.full
            and shutil.which("screencapture") and shutil.which("sips")):
        cmd_mirror_fast(args)  # macOS tiled diffs; elsewhere full frames
    else:
        {"mirror": cmd_mirror, "keys": cmd_keys,
         "play": cmd_play}[args.cmd](args)


if __name__ == "__main__":
    main()

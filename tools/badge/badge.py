#!/usr/bin/env python3
"""Drive the badge over the USB console: press buttons, capture the screen.

This exists so a change to the UI can be checked without a human looking at the
badge. `press` has been injectable for a while, but there was no way to observe
the result short of pointing a webcam at the thing. Now:

    badge.py shot screen.png
    badge.py press Right press A settle 500 shot after.png
    badge.py tree                       # what the UI *is*, not what it looks like
    badge.py run examples/launcher-tour.txt

Verbs run left to right in a single invocation, over one open port, because
opening the port per action is both slow and a good way to race the console.

Stdlib only, on purpose — no venv, no pip, same as tools/check-bbx-proto.py.
termios instead of pyserial; the badge is USB-CDC so the baud rate is fiction
anyway and all that matters is putting the tty in raw mode.

Two protocol traps, both learned the hard way (see tools/badge-provision/README.md):
  * Send a bare CR. The console VFS is ESP_LINE_ENDINGS_CR on RX, so a "\\r\\n"
    delivers two line terminators and the second one lands in the next command.
  * Never write more than ~256 bytes in one burst. The USB-Serial-JTAG RX ring
    is 256 bytes and cannot be grown; the tail is dropped silently.
"""

import argparse
import codecs
import glob
import os
import random
import re
import select
import shlex
import struct
import sys
import termios
import time
import zlib

PROMPT = "badge> "

# ESP_LOG lines: "I (12345) TAG: message". These interleave with command output
# at any time — NimBLE logs a GAP line every advertising interval — so they get
# filtered out of every response rather than being allowed to corrupt one.
LOG_LINE = re.compile(r"^[IWEDV] \(\d+\) [^:]{1,32}: ")


def strip_logs(text):
    return "\n".join(
        l for l in text.split("\n") if not LOG_LINE.match(l.lstrip("\x00"))
    )


def _token(kind):
    return f"__{kind}_{random.randrange(1 << 32):08x}"

BUTTONS = ["A", "B", "Home", "Down", "Left", "Right", "Up", "Aux1", "Start"]

# Host->badge chunking, to stay under the badge's 256-byte USB RX ring.
WRITE_CHUNK = 128
WRITE_PAUSE = 0.02


# ---------------------------------------------------------------------------
# transport


class Badge:
    def __init__(self, path, debug=False):
        self.path = path
        self.debug = debug
        self.rx = ""
        # Incremental decode: a multibyte UTF-8 char (em dashes in label
        # text, icon glyphs) can straddle two reads. Decoding each chunk
        # separately with "replace" turned those into U+FFFD and made
        # identical screens diff against each other.
        self._decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            self._raw_mode()
        except Exception:
            os.close(self.fd)
            raise

    def _raw_mode(self):
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(self.fd)
        iflag &= ~(
            termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP
            | termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON
        )
        oflag &= ~termios.OPOST
        lflag &= ~(
            termios.ECHO | termios.ECHONL | termios.ICANON
            | termios.ISIG | termios.IEXTEN
        )
        cflag &= ~(termios.CSIZE | termios.PARENB)
        # CLOCAL: this is USB-CDC, there is no carrier to wait for.
        cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        termios.tcsetattr(
            self.fd, termios.TCSANOW,
            [iflag, oflag, cflag, lflag, ispeed, ospeed, cc],
        )
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def close(self):
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def write(self, data):
        for i in range(0, len(data), WRITE_CHUNK):
            chunk = data[i:i + WRITE_CHUNK]
            while chunk:
                select.select([], [self.fd], [], 2.0)
                n = os.write(self.fd, chunk)
                chunk = chunk[n:]
            if i + WRITE_CHUNK < len(data):
                time.sleep(WRITE_PAUSE)

    def sendline(self, s):
        # Bare CR. See the module docstring.
        self.write((s + "\r").encode())

    def _pump(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return False
        data = os.read(self.fd, 4096)
        if not data:
            return False
        self.rx += self._decoder.decode(data)
        return True

    def read_until(self, needle, timeout=10.0, stall=3.0):
        """Read until `needle` appears, then consume through it and return the
        text before it. `stall` bounds the gap between reads so a wedged badge
        fails in seconds rather than at the full timeout."""
        deadline = time.time() + timeout
        last = time.time()
        while True:
            idx = self.rx.find(needle)
            if idx >= 0:
                out = self.rx[:idx]
                self.rx = self.rx[idx + len(needle):]
                return out
            now = time.time()
            if now > deadline:
                raise TimeoutError(f"timed out waiting for {needle!r}")
            if now - last > stall:
                raise TimeoutError(f"badge went quiet waiting for {needle!r}")
            if self._pump(0.2):
                last = time.time()

    def echo_token(self, token, timeout, stall):
        """Make the badge say `token` back to us.

        `press <token>` is the echo primitive: an unrecognised button name is a
        no-op that prints the name back, and it exists in any firmware that has
        the console at all. Nothing else in the command set echoes caller-
        supplied text.
        """
        self.sendline(f"press {token}")
        return self.read_until(f"unknown button: {token}",
                               timeout=timeout, stall=stall)

    def sync(self, attempts=3):
        """Drive the console to a known point rather than hoping for a prompt.

        Connecting mid-stream is the normal case, not the exception: a previous
        session may have swallowed the prompt, the badge may be mid-boot dumping
        a screen of init logs, and NimBLE chatters continuously. Waiting for
        ambient output is therefore unreliable in both directions — it can hang
        forever, or return instantly with megabytes of stale backlog.

        So don't read what's already there. Emit a token only this process knows
        and read *forward* until it comes back, discarding everything before it.
        Whatever the badge was saying when we arrived stops mattering.
        """
        for _ in range(attempts):
            self.write(b"\x03")      # abandon any half-typed line
            self.rx = ""
            self._decoder.reset()
            try:
                self.echo_token(_token("sync"), timeout=6.0, stall=6.0)
                self.read_until(PROMPT, timeout=2.0, stall=2.0)
                self.rx = ""
                self._decoder.reset()
                return
            except TimeoutError:
                continue
        raise SystemExit(
            f"no response from {self.path} — is the badge running this firmware, "
            f"and is anything else (idf.py monitor, serial-buttons) holding the port?"
        )

    def command(self, cmd, timeout=30.0, stall=5.0):
        """Run one console command and return exactly what it printed.

        The response is framed by a trailing token rather than by the prompt, so
        output is delimited by something we chose. A prompt is ambient and can
        be swallowed, duplicated, or arrive interleaved with a log line; a token
        cannot be any of those.
        """
        end = _token("end")
        self.rx = ""
        self._decoder.reset()
        self.sendline(cmd)
        raw = self.echo_token(end, timeout=timeout, stall=stall)

        # Drain through the prompt that follows the token, so the stream is left
        # at a command boundary. Clearing self.rx at the top is not enough on its
        # own: bytes already in flight aren't in the buffer yet, and would arrive
        # at the head of the *next* response. (The token command returns
        # non-zero, so the REPL also prints an error line here.)
        try:
            self.read_until(PROMPT, timeout=5.0, stall=5.0)
        except TimeoutError:
            pass

        # raw is: echo(cmd) + output + prompt + echo("press <end>"). Everything
        # from the last prompt on belongs to the framing, not to the command.
        cut = raw.rfind(PROMPT)
        if cut >= 0:
            raw = raw[:cut]

        lines = raw.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        if lines and lines[0].strip() == cmd.strip():
            lines = lines[1:]
        out = strip_logs("\n".join(lines)).strip("\n")
        if self.debug:
            sys.stderr.write(f"--- {cmd}\n{out}\n---\n")
        return out


def find_port(explicit=None):
    if explicit:
        return explicit
    env = os.environ.get("BADGE_PORT")
    if env:
        return env
    candidates = sorted(
        glob.glob("/dev/tty.usbmodem*")      # macOS
        + glob.glob("/dev/ttyACM*")          # Linux
    )
    if not candidates:
        raise SystemExit(
            "no badge found. Plug it in, or pass --port / set BADGE_PORT."
        )
    if len(candidates) > 1:
        listing = "\n  ".join(candidates)
        raise SystemExit(f"several ports match, pick one with --port:\n  {listing}")
    return candidates[0]


# ---------------------------------------------------------------------------
# screen capture: rle1 + base64 RGB565 -> PNG
#
# Wire format, emitted by hal_screencap.cpp:
#
#   SHOT <w> <h> rgb565le rle1
#   S <x1> <y1> <x2> <y2>
#   <base64 lines, 76 cols>
#   S ...
#   END bytes=.. raw=.. crc32=.. stripes=.. covered=.. elapsed_ms=..
#
# A stripe's base64 runs until the next "S " or "END" line. The RLE decoder
# knows it is done when it has produced exactly the stripe's pixel count.


class ShotError(Exception):
    pass


def rle1_decode(data, want_px):
    """PackBits over 16-bit little-endian pixels. Control byte:
       0x00..0x7F -> (n+1) literal pixels follow
       0x80..0xFF -> run of (n-0x80+2) copies of the next pixel"""
    out = bytearray()
    i = 0
    n = len(data)
    limit = want_px * 2
    while len(out) < limit:
        if i >= n:
            raise ShotError(
                f"rle1 ran out of input: {len(out)//2} of {want_px} pixels"
            )
        ctrl = data[i]
        i += 1
        if ctrl & 0x80:
            count = (ctrl - 0x80) + 2
            if i + 2 > n:
                raise ShotError("rle1 truncated run")
            out += data[i:i + 2] * count
            i += 2
        else:
            count = ctrl + 1
            if i + count * 2 > n:
                raise ShotError("rle1 truncated literal")
            out += data[i:i + count * 2]
            i += count * 2
    if len(out) != limit:
        raise ShotError(
            f"rle1 overran: {len(out)//2} pixels for a {want_px}-pixel stripe"
        )
    if i != n:
        raise ShotError(f"rle1 left {n - i} trailing bytes")
    return bytes(out)


def parse_shot(text):
    """Return (w, h, framebuffer_rgb565le, stats). Raises ShotError."""
    import base64

    lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    w = h = None
    fb = None
    stripes = []          # (x1, y1, x2, y2, b64 chunks)
    cur = None
    end = None

    for line in lines:
        line = line.strip()
        if not line:
            continue
        if line.startswith("SHOT "):
            parts = line.split()
            if len(parts) < 5 or parts[3] != "rgb565le" or parts[4] != "rle1":
                raise ShotError(f"unsupported shot header: {line!r}")
            w, h = int(parts[1]), int(parts[2])
            fb = bytearray(w * h * 2)
        elif line.startswith("S "):
            parts = line.split()
            if len(parts) != 5:
                raise ShotError(f"bad stripe header: {line!r}")
            cur = [int(v) for v in parts[1:5]] + [[]]
            stripes.append(cur)
        elif line.startswith("END "):
            end = dict(
                kv.split("=", 1) for kv in line[4:].split() if "=" in kv
            )
            cur = None
        elif line.startswith("#"):
            continue
        elif cur is not None:
            cur[4].append(line)

    if w is None:
        raise ShotError("no SHOT header — did the firmware get flashed?")
    if end is None:
        raise ShotError("no END trailer — capture was cut short")

    raw_stream = bytearray()   # stripe payloads in emission order, for the CRC
    covered = 0
    for x1, y1, x2, y2, chunks in stripes:
        sw, sh = x2 - x1 + 1, y2 - y1 + 1
        if sw <= 0 or sh <= 0:
            raise ShotError(f"degenerate stripe {x1},{y1},{x2},{y2}")
        payload = base64.b64decode("".join(chunks), validate=True)
        px = rle1_decode(payload, sw * sh)
        raw_stream += px
        covered += sw * sh
        for row in range(sh):
            dst = ((y1 + row) * w + x1) * 2
            src = row * sw * 2
            fb[dst:dst + sw * 2] = px[src:src + sw * 2]

    want_crc = int(end.get("crc32", "-1"), 16)
    got_crc = zlib.crc32(bytes(raw_stream)) & 0xFFFFFFFF
    if want_crc != got_crc:
        raise ShotError(f"crc mismatch: badge {want_crc:08x}, host {got_crc:08x}")
    if covered != w * h:
        raise ShotError(f"incomplete frame: {covered} of {w*h} pixels")

    return w, h, bytes(fb), end


# 5- and 6-bit channel expansion, precomputed.
_C5 = bytes((v * 255 + 15) // 31 for v in range(32))
_G6 = bytes((v * 255 + 31) // 63 for v in range(64))


def rgb565_to_rgb888(fb, w, h, invert=False):
    out = bytearray(w * h * 3)
    for i in range(w * h):
        v = fb[i * 2] | (fb[i * 2 + 1] << 8)
        if invert:
            v ^= 0xFFFF
        out[i * 3] = _C5[(v >> 11) & 0x1F]
        out[i * 3 + 1] = _G6[(v >> 5) & 0x3F]
        out[i * 3 + 2] = _C5[v & 0x1F]
    return bytes(out)


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    stride = w * 3
    raw = bytearray()
    for y in range(h):
        raw += b"\x00"                      # filter type 0 (None)
        raw += rgb[y * stride:(y + 1) * stride]

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


# ---------------------------------------------------------------------------
# verbs

# verb -> number of arguments it consumes
VERBS = {
    "press": 1,
    "settle": 1,
    "shot": 1,
    "tree": "?",       # optional path; prints to stdout without one
    "console": 1,
    "expect": 1,
    "run": 1,
}


def do_press(badge, name, args):
    match = [b for b in BUTTONS if b.lower() == name.lower()]
    if not match:
        raise SystemExit(f"unknown button {name!r}; one of: {', '.join(BUTTONS)}")
    out = badge.command(f"press {match[0]}")
    if out.strip():
        print(out.strip())


def do_settle(badge, ms, args):
    time.sleep(int(ms) / 1000.0)


def out_path(path, args):
    """Artefact paths are relative to --out-dir, so a script can be run from
    anywhere without rewriting the paths inside it."""
    if os.path.isabs(path):
        return path
    return os.path.join(args.out_dir, path)


def do_shot(badge, path, args):
    path = out_path(path, args)
    t0 = time.time()
    text = badge.command("shot", timeout=args.timeout, stall=args.timeout)
    try:
        w, h, fb, end = parse_shot(text)
    except ShotError as e:
        raise SystemExit(f"shot failed: {e}")
    rgb = rgb565_to_rgb888(fb, w, h, invert=args.invert)
    write_png(path, w, h, rgb)
    wire = int(end.get("bytes", 0))
    print(
        f"wrote {path} {w}x{h} "
        f"({wire/1024:.1f} KB wire, badge {end.get('elapsed_ms','?')} ms, "
        f"total {(time.time()-t0)*1000:.0f} ms)"
    )


def do_tree(badge, path, args):
    out = badge.command("uitree", timeout=args.timeout, stall=args.timeout)
    args.last_output = out
    if path:
        path = out_path(path, args)
        with open(path, "w") as f:
            f.write(out.rstrip() + "\n")
        nodes = "?"
        for line in out.splitlines():
            if line.startswith("END "):
                nodes = dict(
                    kv.split("=", 1) for kv in line[4:].split() if "=" in kv
                ).get("nodes", "?")
        print(f"wrote {path} ({nodes} nodes)")
    else:
        print(out.rstrip())


def do_console(badge, cmd, args):
    out = badge.command(cmd, timeout=args.timeout, stall=args.timeout)
    args.last_output = out
    if out.strip():
        print(out.rstrip())


def do_expect(badge, needle, args):
    """Assert the last tree/console output contained `needle`. This is what
    makes a script a check rather than just a macro."""
    haystack = getattr(args, "last_output", None)
    if haystack is None:
        raise SystemExit("expect: nothing to match against yet — run tree or console first")
    if needle not in haystack:
        sys.stderr.write(haystack.rstrip() + "\n")
        raise SystemExit(f"expect failed: {needle!r} not in the last output")
    print(f"ok: found {needle!r}")


HANDLERS = {
    "press": do_press,
    "settle": do_settle,
    "shot": do_shot,
    "tree": do_tree,
    "console": do_console,
    "expect": do_expect,
}


def read_script(path, seen):
    """Expand `run <script>` into its verbs at parse time, so a script and a
    command line are the same program by the time anything talks to the badge."""
    real = os.path.realpath(path)
    if real in seen:
        raise SystemExit(f"run: {path} includes itself")
    seen = seen | {real}
    try:
        with open(path) as f:
            lines = f.readlines()
    except OSError as e:
        raise SystemExit(f"run: {e}")

    tokens = []
    for n, line in enumerate(lines, 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            tokens += shlex.split(line)
        except ValueError as e:
            raise SystemExit(f"{path}:{n}: {e}")
    return parse_program(tokens, seen)


def parse_program(tokens, seen=frozenset()):
    """Turn a flat token list into [(verb, arg), ...]."""
    program = []
    i = 0
    while i < len(tokens):
        verb = tokens[i]
        if verb not in VERBS:
            raise SystemExit(
                f"unknown verb {verb!r}; one of: {', '.join(sorted(VERBS))}"
            )
        need = VERBS[verb]
        arg = None
        if need == "?":
            # Optional argument: the next token counts as one only if it isn't
            # itself a verb. Write ./shot for a file that shares a verb's name.
            if i + 1 < len(tokens) and tokens[i + 1] not in VERBS:
                arg = tokens[i + 1]
                i += 1
        elif need:
            if i + 1 >= len(tokens):
                raise SystemExit(f"{verb} needs an argument")
            arg = tokens[i + 1]
            i += 1
        i += 1

        if verb == "run":
            program += read_script(arg, seen)
        else:
            program.append((verb, arg))
    return program


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="verbs: " + "  ".join(
            f"{v} [arg]" if n == "?" else (f"{v} <arg>" if n else v)
            for v, n in sorted(VERBS.items())
        ),
    )
    parser.add_argument("--port", help="serial device (default: autodetect)")
    parser.add_argument("--timeout", type=float, default=30.0,
                        help="seconds to wait for a command (default 30)")
    parser.add_argument("--invert", action="store_true",
                        help="invert captured colours (only if the panel's "
                             "INVON turns out to apply to the LVGL buffer)")
    parser.add_argument("--out-dir", default=".", metavar="DIR",
                        help="where shot/tree write their files (default .)")
    parser.add_argument("--debug", action="store_true",
                        help="echo raw console traffic to stderr")
    parser.add_argument("tokens", nargs="+", metavar="VERB",
                        help="one or more verbs with their arguments")
    args = parser.parse_args()

    args.last_output = None
    program = parse_program(args.tokens)
    if not os.path.isdir(args.out_dir):
        raise SystemExit(f"--out-dir {args.out_dir!r} is not a directory")
    port = find_port(args.port)

    with Badge(port, debug=args.debug) as badge:
        badge.sync()
        for verb, arg in program:
            HANDLERS[verb](badge, arg, args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

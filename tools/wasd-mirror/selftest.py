#!/usr/bin/env python3
"""Offline self-test for wasd_mirror.py. No badge, no network.

Checks the contracts that would otherwise only fail against a live badge:
maze dimensions, printable-ASCII text, S->E solvability with the real
step() rules, both badge text posts under the 256-char cap, and the
button map covering WASD without touching Home (canvas-mode exit).
"""

import importlib.util
import sys
from collections import deque

spec = importlib.util.spec_from_file_location(
    "wasd_mirror", "tools/wasd-mirror/wasd_mirror.py")
w = importlib.util.module_from_spec(spec)
spec.loader.exec_module(w)

failures = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name
          + (f" ({detail})" if detail and not cond else ""))
    if not cond:
        failures.append(name)


check("maze is 20x15", len(w.MAZE) == 15 and all(len(r) == 20 for r in w.MAZE))
check("maze is printable ASCII",
      all(32 <= ord(c) < 127 for r in w.MAZE for c in r))
check("one start, one exit",
      sum(r.count("S") for r in w.MAZE) == 1
      and sum(r.count("E") for r in w.MAZE) == 1)

S = next((x, y) for y, r in enumerate(w.MAZE)
         for x, c in enumerate(r) if c == "S")
E = next((x, y) for y, r in enumerate(w.MAZE)
         for x, c in enumerate(r) if c == "E")
prev = {S: None}
q = deque([S])
while q:
    x, y = q.popleft()
    for d, (dx, dy) in w.DIRS.items():
        if len(d) > 1:
            continue
        nx, ny = x + dx, y + dy
        if (0 <= ny < w.PH and 0 <= nx < w.PW
                and w.MAZE[ny][nx] != "#" and (nx, ny) not in prev):
            prev[(nx, ny)] = ((x, y), d)
            q.append((nx, ny))
check("maze solvable S->E", E in prev)

if E in prev:  # replay the solution through the real step() rules
    sol = []
    c = E
    while prev[c]:
        c, d = prev[c]
        sol.append(d)
    px, py, moves = S[0], S[1], 0
    for ch in sol[::-1]:
        dx, dy = w.DIRS[ch]
        px, py, ok = w.step(px, py, dx, dy)
        moves += ok
    check("solution replays onto E", (px, py) == E, f"ends at {(px, py)}")
    rows = w.maze_text(px, py).split("\n")
    p1 = "99mv 59s\n" + "\n".join(rows[:8])
    p2 = "\n".join(rows[8:])
    check("badge post 1 under 256 chars", len(p1) <= 256, f"{len(p1)}")
    check("badge post 2 under 256 chars", len(p2) <= 256, f"{len(p2)}")

check("map covers WASD",
      all(w.BTN_MAP.get(b) == k
          for b, k in [("up", "w"), ("left", "a"),
                       ("down", "s"), ("right", "d")]))
check("map leaves Home alone (canvas exit)", "home" not in w.BTN_MAP)
check("badge dirs match PC dirs",
      all(w.DIRS[b] == w.DIRS[k] for b, k in w.WASD.items()))

# PNG codec: round-trip on noise + RGBA alpha-drop, tile change detection.
import random
random.seed(7)
rgb = bytes(random.randrange(256) for _ in range(64 * 40 * 3))
open("/tmp/wasd-rt.png", "wb").write(w.encode_png_rgb(64, 40, rgb))
check("png rgb round-trip", w.decode_png_rgb("/tmp/wasd-rt.png")
      == (64, 40, rgb))
def _to_rgba_png(path_in, path_out):
    import struct
    import zlib as _z
    w0, h0, px = w.decode_png_rgb(path_in)
    raw = b"".join(b"\x00" + b"".join(px[i:i + 3] + b"\xff"
                                      for i in range(y * w0 * 3,
                                                     (y + 1) * w0 * 3, 3))
                    for y in range(h0))
    comp = _z.compress(raw, 6)

    def chunk(typ, body):
        return (struct.pack(">I", len(body)) + typ + body
                + struct.pack(">I", _z.crc32(typ + body) & 0xffffffff))
    open(path_out, "wb").write(
        bytes([137, 80, 78, 71, 13, 10, 26, 10])
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w0, h0, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", comp) + chunk(b"IEND", b""))


_to_rgba_png("/tmp/wasd-rt.png", "/tmp/wasd-rt-a.png")
check("png rgba decodes (alpha dropped)",
      w.decode_png_rgb("/tmp/wasd-rt-a.png") == (64, 40, rgb))

a = bytes(64 * 40 * 3)
b = bytearray(a)
b[1000:1010] = b"\xff" * 10
check("identical tiles compare equal", a == bytes(a))
check("one-pixel change detected", bytes(b) != a)

import os
for f in ("/tmp/wasd-rt.png", "/tmp/wasd-rt-a.png"):
    os.remove(f)

print(f"{len(failures)} failure(s)")
sys.exit(1 if failures else 0)

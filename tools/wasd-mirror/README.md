# wasd-mirror — badge as screen + WASD controller

Host-side game setup for the HTN OS proxy firmware (badge ↔ server over
`https://badge.solana-htn.com`, no re-flashing). One script, stdlib only,
no pip — same rule as `tools/badge/badge.py`.

```sh
export BADGE_ID=xb2b9 BADGE_KEY=hunter2
tools/wasd-mirror/wasd_mirror.py mirror --fps 1   # PC screen -> badge screen
tools/wasd-mirror/wasd_mirror.py keys             # badge buttons -> PC WASD keys
tools/wasd-mirror/wasd_mirror.py play             # demo maze, badge mirrors it
```

`--base` / `$BADGE_BASE` points at a local service
(`http://localhost:3100`) instead of production.

## Modes

| Mode | Direction | How |
|---|---|---|
| `mirror` | PC screen → badge | macOS default is **fast tiled diffs**: one full keyframe, then only changed 64×40 px tiles via `POST /image?x=&y=&fit=none` in parallel (stdlib PNG codec, token bucket holds 14 cmds/s against the 20/s limit; keyframe every 8 s heals misses). A full-screen blit takes ~1 s on the wire, so full frames cap at ~1 fps — tiles are a few KB and refresh moving regions in a fraction of that. `--full` forces the old full-frame JPEG path (also the fallback on Linux, which has no `sips`). |
| `keys` | badge buttons → PC | `GET /events` SSE, press edges only. macOS types via `osascript`/System Events (needs Accessibility permission), Linux via `xdotool` when present, otherwise prints — `--no-inject` forces print mode. |
| `play` | both | A 20×15 maze runs in the terminal and is mirrored to the badge with two `POST /text` commands per move (2 cmds/move, far under the 20/s budget). Move with PC WASD on stdin or the badge D-pad. Reach `E` to win. |

Button map: `Up→w Left→a Down→s Right→d`, plus `A→space B→escape
Start→enter`. `--wasd-only` drops the extras. Home is never bound — the
owner holds it for 1 s to leave canvas mode.

## Offline check

No badge needed — validates the maze (dims, solvable, beats the 256-char
text cap) and the button map:

```sh
tools/wasd-mirror/selftest.py
```

# September 8 badge QA

My Badge opens on the QR code. **A** opens QR customization; **Start** opens
identity details. Finishing onboarding consumes the Start release in the old app,
so the first My Badge screen stays on its QR code.

The Solana Scavenger Hunt shows instructions on first entry. **Start** reopens
instructions, **A** opens the PAL inventory while scanning, and **B** also opens
inventory (including when the radio fails). The 3×3 picker groups Ginny, Patch,
and Vinyl into rows, shows locked PALs in gray, and has a separate **None** row.
Use the D-pad to choose, **A** to save, or **B** to cancel. The selection is shared
with My Badge and persists in the existing Solana wallet file. This changes the
presentation; boxes still use the existing BLE relay protocol.

The default launcher order is:

1. My Badge
2. Connect
3. Scanner
4. Share
5. Solana Scavenger Hunt
6. Sponsors
7. Workshops
8. Mood LEDs
9. Socials
10. Schedule
11. Info & Rules
12. Museum
13. Progress
14. Snake
15. Dice

Lightshow, EightBall, Tetris, Breakout, Tilt Maze, and Flappy Goose remain public
unlockable apps. The public/all provisioning modes still include them. Factory
reset or `appmode default <admin-password>` restores the default bundle. Untouched
older default launchers migrate Info next to Museum; custom orders and existing
unlocks are preserved. Progress counts the nine known PALs, excluding unknown
future reward IDs from that denominator.

Three brief green LED pulses now celebrate new PAL rewards, new app and sponsor
unlocks, Brick Breaker board clears, Tic-Tac-Toe wins, completed Tilt Mazes,
four-line Tetris clears, every ten Snake foods and Flappy pipes, Snake records,
maximum dice rolls, and completed Bubble Wrap sheets. The existing LED HAL caps
brightness and restores the app's requested frame after the pulses.

All builds include the authenticated admin commands. See the
[provisioning protocol](../tools/badge-provision/README.md#admin-password).
The generated Admin password is not committed; operators enter it in a masked
field in both host UIs. The firmware retains its SHA-256 verifier, and the
dashboard test checks that neither UI ships a default. Registration preflights
authorization before writing identity, and engineer flashing supplies the same
password after boot.
PR firmware builds and the deployed engineer flasher both use ESP-IDF 5.5.3.

## Icon sources

The built-in imagegen tool produced the replacement PNG masters, resized to
96×96 while retaining alpha. The existing asset manifest converts them to the
48×48 launcher images. Level continues to use its existing PNG with the shared
white-outline renderer. PAL thumbnails are generated deterministically from the
existing PAL masters through the same manifest.

- `assets/icons/eightball.png`: “Clean up the stray pixels poking out around the
  ball: make its outside contour a regular, clean circular pixel-art silhouette.
  Preserve the simple black/dark-charcoal ball, clear white 8 on a small pale
  circular face. Remove sparkles or stray marks outside the ball. Transparent
  RGBA background, no shadow or glow, no backdrop. Centered square icon with
  8 percent transparent padding; designed to remain recognizable downscaled to
  48x48. Thin continuous white outline around the ball so it reads against black.
  Keep the charming blocky pixel style but make the digit unmistakably 8.”
- `assets/icons/progress.png`: “One circular progress ring with roughly 75 percent
  completed in bright mint teal (#2fe2c0), the remaining quarter in muted blue
  gray. A simple white checkmark centered inside the ring. Clean geometric flat
  icon with consistent thick strokes, legible at 48x48 pixels. Centered on a
  square TRUE transparent RGBA background with generous 10 percent padding.
  No bars, no numerals, no lettering, no decorative marks, no glow, no shadow,
  no backdrop. High contrast for a black badge screen.”

## Validation

`tools/badge-qa/run.py` renders the real LVGL apps with host stubs for hardware
and checks state, file persistence, button handoff, and label bounds.
`tools/test-admin-console.py` tests the actual handlers and crypto verifier using
a test-only credential. `tools/test-admin-dashboard.mjs` runs the dashboard's
programming flow against a deterministic transport. These host checks do not
exercise a physical badge or over-the-air redemption; firmware builds and size
checks run in the PR workflow.

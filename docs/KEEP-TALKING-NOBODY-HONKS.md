# Keep Talking and Nobody Honks

Keep Talking and Nobody Honks (KTNH) is a group game for at least two people:
one Operator with a badge and one or more Experts with the manual on a phone.
It borrows the describe-and-decode rhythm of a
bomb-defusal game without copying its manual, language, or module rules.

## Player contract

- The **Operator** holds the badge and is the only person who can see its
  puzzles.
- The **Experts** scan `https://badge.hackthenorth.com/ktnh` and are the only
  people who should read the decoder manual.
- The Operator describes what is on the badge. Experts look up the rule and
  talk the Operator through the input.
- A run contains 1-6 different modules, selected in the ready room. The badge
  shuffles the module order for every run.
- The timer starts at `60 + 30 x module count` seconds: 1:30 for one module,
  up to 4:00 for six.
- A wrong submission removes ten seconds and resets only the current module's
  input. The puzzle itself does not change.
- Solving every module before time expires wins. There are no network calls,
  accounts, saved scores, or event-critical state.

HOME always leaves the app. During a run, B clears the current partial input.
START opens a numbered module selector: D-pad chooses, A opens an unsolved
module, and B or START returns to the current puzzle. Switching preserves
puzzles and partial answers; the countdown keeps running in the selector.
On result screens, START immediately rematches at the same difficulty and B
returns to the ready room.

## Flow

1. **Briefing:** explain the two roles and the no-peeking rule.
2. **Ready room:** display the mobile manual QR, select 1-6 modules with
   Left/Right, and wait for START so the group controls the countdown start.
3. **Run:** show the remaining time, two-digit unit code, progress, and one
   active module. The badge shows module numbers rather than their names, so
   Experts identify puzzles from the Operator's description and the manual's
   example screenshots. The unit code is shared context used by several rules.
4. **Result:** show either `DEFUSED` with remaining time or `TOO MANY HONKS`,
   then offer rematch/setup controls.

## Modules

Each module is generated once when first opened and can be solved in any order.
Inputs use the D-pad, A, B, and START on the 320x240 screen.

### Patch Cables

Four coloured cables are numbered 1-4. The manual chooses a cable using unit
code parity and colour precedence. Left/Right selects; A cuts.

### Glyph Stack

Four ASCII glyphs come from one ordered manual row. Each glyph appears in two
rows, and pairs of rows share two glyphs in opposite orders. Every four-glyph
set still matches exactly one row. Experts identify that row and give the
left-to-right order. Left/Right selects; A presses a glyph.

### Frequency Dial

A colour name and the unit code select one digit in a compact lookup table.
Left/Right changes the dial; A submits.

### Arrow Relay

The badge shows three directions. Experts transform them using the tens digit
of the unit code. The Operator enters the answer directly on the D-pad.

### Word Lock

A code word and unit-code parity map to a three-digit combination. Left/Right
moves between digits, Up/Down edits, and A submits.

### Simon Honks

The badge loops a three-colour signal. Experts transform colours into D-pad
directions using the unit code's last digit. A restarts the visual loop.

## Technical boundaries

- KTNH is a `Public` built-in app with slug `ktnh`.
- The web manual is a first-party route in the repository's existing Vite site,
  not a PDF or external document.
- Randomness is gameplay-only. It uses a small local PRNG seeded from
  `esp_random()` and the LVGL tick; it is not security-sensitive.
- The active countdown requests a wake lock. Leaving the app discards the run,
  and no state is written to flash.
- No radio, NFC, sound, or backend is required, so a failure cannot affect
  provisioning, networking, sponsor tracking, or any other badge experience.

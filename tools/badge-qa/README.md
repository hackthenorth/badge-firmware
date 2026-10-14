# Host badge UI QA

Run from the repository root with Python 3, CMake, Clang, and the project's
managed LVGL component installed:

```sh
python3 tools/badge-qa/run.py --out-dir /tmp/badge-qa
```

This builds a host executable with real LVGL, AddressSanitizer, and the production
config file reader/writer redirected to a temporary directory. It does not build
or flash ESP32 firmware and does not use a serial port. Hardware and the app
registry are stubbed. `build.log` and rendered 320×240 PPM screens are retained
under the output directory's `badge-ui-qa` child; omit `--out-dir` to use temporary
files that are removed after the run.

Checks cover the onboarding any-button welcome, back navigation,
completing/skipping the button test, shake and agreement gates,
the production LED HAL's physical output (including
gamma, the 26% cap, success pulses and restored app frames), role LEDs on
onboarding page 2, unlit welcome and QR pages, and the same behaviour after
a separate process restart, a single-line rules URL, Mood
LEDs introduction/help, default launcher order/reset/migration, preserved custom
orders and unlocks (including Blackjack on public-mode upgrades), diagnostic
outline rendering, and Bubble Wrap's immediate response plus coalesced all-time
counter writes across sheets, app exits, and a separate process restart. Screenshots
also verify that the Sponsor Map paints its floor immediately and adds booth
objects incrementally. Physical LED visibility still needs a flashed badge.

The harness also checks the Hunt's first-use instructions and help, the grouped
pal picker (locked/equipped/None), QR defaults, real wallet persistence and failed
saves, Progress, game celebrations, and held-button suppression at app transitions.
Connect checks cover the fixed one-second A-button handshake and ACK transport
grace, stale packets, the -60 dBm / 5 cm guidance and radio gates, unique contact
counting, duplicate-contact feedback, Contacts controls, and combined-axis bumps.
It uses the activated ESP-IDF cJSON sources (`IDF_PATH`, or the standard local
5.5.3 installation) and host libsodium (`brew install libsodium` on macOS,
`apt install libsodium-dev` on Linux). Radio and random input are deterministic
host stubs; this does not exercise over-the-air box redemption.

Admin protocol checks are separate and use a test-only credential:

```sh
python3 tools/test-admin-console.py
node tools/test-admin-dashboard.mjs
```

The physical-button debounce helper has a focused sanitizer-backed test:

```sh
make -C tools/button-debounce-test check
```


Game regressions cover Sokoban after 5,000 extra moves and undoing a win, Pong's
full court and first-to-five loss, Dance's five-miss limit, Etch pen toggle,
Hopper level persistence, a Minesweeper loss versus a zero-second clear, actual
2048 tile persistence, Flappy saving an unfinished run when paused, Blackjack
rounds, and Flashlight's public access. The registry harness verifies that every
app gets a progress checkpoint before Home pause and exit.
Chess and Share exercise the real engines with simulated radio delivery for
mutual A intent, 1001 ms rejection, wrong game/offer/role and weak signals.
Changed Chess, Share and Sync UI sources also receive host syntax checks.

Outrun, Sponsors, and Etch also cycle through repeated entry, use, and exit
under AddressSanitizer. The harness checks that native heap usage returns to
its warmed baseline, forces their session allocations to fail, and verifies
that input, cleanup, and the following successful visit remain safe.

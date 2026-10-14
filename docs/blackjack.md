# Blackjack

The public `blackjack` app uses the usual badge unlock flow and a text `21`
launcher icon. Default-mode badges unlock it from an NFC text payload containing
exactly `blackjack`; badges provisioned in `public` or `all` mode include it
automatically, including badges upgraded from firmware that predates the app.
Each round shuffles a fresh 52-card deck. A hits, B stands, and A or Start deals
again after a result. Home uses the shared game exit prompt.

Aces count as 11 or 1, face cards as 10. The dealer stands on all 17s, including
soft 17. Initial blackjacks resolve immediately; reaching 21 later automatically
stands. A player bust loses immediately. Equal non-bust totals count as a
player win, including two natural blackjacks. No betting, splitting or doubling.

Completed rounds save `wins`, `losses`, and `tie_wins` through `hal_config` to
`config/blackjack.cfg`. `wins` includes `tie_wins`; unfinished rounds do not
change the totals. These fields are part of the existing snapshot upload and
can be displayed by the dashboard without changing the API or credentials.

Run `make -C tools/blackjack-test check` for host rules tests, or
`python3 tools/badge-qa/run.py --out-dir /tmp/blackjack-qa` for real LVGL layout,
button and persistence checks. These do not build or flash ESP32 firmware.
Before the first firmware build, run `idf.py reconfigure` to discover the new
app source, then the normal build/flash workflow.

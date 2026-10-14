#pragma once

#include <cstddef>
#include <cstdint>

class App;
App *app_chess_instance();

// Console hooks, backing the `chess` command family in hal_console.cpp. The
// console REPL runs on its own task, so every one of these takes the LVGL lock
// itself rather than trusting the caller — the app's state and the widgets
// showing it are mutated together and must not tear.
//
// All return false if the chess app is not currently foregrounded.

// One-line-per-topic dump of everything the headless test harness needs to
// assert on: mode, cursor in both board and screen coordinates, side to move,
// ply, result, FEN, and the LVGL heap headroom the 128-object board consumes.
size_t chess_app_debug(char *out, size_t out_sz);

bool chess_app_set_fen(const char *fen);
bool chess_app_play_uci(const char *uci);
bool chess_app_new_game();
bool chess_app_flip();

// Force the pairing bump window open without shaking the badge. This is how
// duet.py pairs two badges sitting still on a desk.
bool chess_app_bump();

// Snapshots the position under the lock and then runs UNLOCKED: perft at
// depth 4 is seconds of work on an 80 MHz C3, and holding the LVGL lock that
// long would freeze the display and stall the 50 Hz tick. Depth is capped by
// the caller. Returns false if the app is not active or the scratch
// allocation failed.
bool chess_app_perft(int depth, uint64_t *out_nodes, uint32_t *out_ms);

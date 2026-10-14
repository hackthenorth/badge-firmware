#pragma once

// LCD status screen for the sync station: the same ST7789 panel and the same
// LVGL graphics system as the badge, laid out like the badge Sync app
// (title, panel, status + detail, progress bar).
//
// Display is status-only: if it is missing or init fails, everything here
// degrades to a no-op and the station keeps working headless. All functions
// are safe to call from any task; screen_show() never blocks for long.
void screen_init(void);

// line1/line2 are copied and truncated to SCREEN_LINE_MAX chars; NULL blanks
// a line. recv/total draws a progress bar; pass total <= 0 to hide it.
#define SCREEN_LINE_MAX 31
void screen_show(const char *line1, const char *line2, int recv, int total);

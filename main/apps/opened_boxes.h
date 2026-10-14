#pragma once

#include <cstdio>
#include <cstring>

// Boxes this badge has already opened, shared between the badge firmware
// (main/apps/app_blindbox.cpp) and the host test in tools/blindbox-test.
// Deliberately free of LVGL and ESP-IDF so it builds on the host with
// nothing but a C++17 compiler.
//
// One open per badge per box, ever — except the replayable final booth
// below. The proximity re-arm check can't tell "still standing here" from
// "left and came back" — enter_idle() clears the beacon samples, so the
// first tick after A always sees no boxes and reads as "walked away" —
// and fresh app entry is always armed. Failures never record, so a box
// that errored stays immediately retryable.
//
// The firmware persists the set as one space-joined hal_config line, so the
// once-ever promise survives reboots (only a factory reset clears it).
class OpenedBoxes {
public:
    // Capacity: 24 worst-case 39-char ids serialize to 959 bytes, inside
    // hal_config's 1151-byte line budget. Venue ids are relay-01 style.
    static constexpr int kCap = 24;
    static constexpr size_t kIdCap = 40;
    // Join buffer every caller must provide: every slot full-length.
    static constexpr size_t kJoinCap = kCap * kIdCap;

    // Exception to the once-ever rule: the final booth stays replayable —
    // attendees return to it to finish the quest — so it never joins the
    // opened set. Firmware checks this before suppressed()/record().
    static constexpr char kReplayableBox[] = "solana-booth-final";

    static bool replayable(const char *id)
    {
        if (!id || !*id) return false;
        return strcmp(id, kReplayableBox) == 0;
    }

    bool suppressed(const char *id) const
    {
        if (!id || !*id) return false;
        for (const auto &slot : ids_) {
            if (slot[0] != '\0' && strcmp(slot, id) == 0) return true;
        }
        return false;
    }

    // False when full (caller keeps the box scannable) or id empty.
    // Re-recording an opened box is a no-op true.
    bool record(const char *id)
    {
        if (!id || !*id) return false;
        if (suppressed(id)) return true;
        for (auto &slot : ids_) {
            if (slot[0] == '\0') {
                snprintf(slot, sizeof(slot), "%s", id);
                return true;
            }
        }
        return false;
    }

    // Space-joined for one hal_config line. Station ids never contain
    // spaces (relay-01 style), so parse splits on ' ' and skips empties.
    void serialize(char *out, size_t out_sz) const
    {
        if (out_sz == 0) return;
        out[0] = '\0';
        size_t pos = 0;
        for (const auto &slot : ids_) {
            if (slot[0] == '\0') continue;
            int n = snprintf(out + pos, out_sz - pos, "%s%s",
                             pos ? " " : "", slot);
            if (n < 0 || (size_t)n >= out_sz - pos) {
                out[pos] = '\0';  // keep only complete ids
                return;
            }
            pos += (size_t)n;
        }
    }

    // Replaces the set. Overlong tokens truncate (box ids arrive capped at
    // 39 bytes everywhere); anything past capacity is dropped.
    void parse(const char *joined)
    {
        for (auto &slot : ids_) slot[0] = '\0';
        if (!joined) return;
        const char *p = joined;
        while (*p != '\0') {
            while (*p == ' ') p++;
            if (*p == '\0') break;
            const char *end = p;
            while (*end != '\0' && *end != ' ') end++;
            char id[kIdCap];
            size_t len = (size_t)(end - p);
            if (len >= kIdCap) len = kIdCap - 1;
            memcpy(id, p, len);
            id[len] = '\0';
            record(id);
            p = end;
        }
    }

    int count() const
    {
        int n = 0;
        for (const auto &slot : ids_) {
            if (slot[0] != '\0') n++;
        }
        return n;
    }

private:
    char ids_[kCap][kIdCap] = {};
};

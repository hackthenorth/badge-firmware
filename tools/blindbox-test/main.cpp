// Host test harness for the blindbox opened-box set: one open per badge
// per box, ever — except the replayable final booth.
//
// opened_boxes.h is deliberately free of LVGL and ESP-IDF so it builds here
// with nothing but a C++17 compiler — the same arrangement as
// tools/share-test. Run this before touching the proximity trigger: a
// re-arm bug found in 200 ms beats the same bug found through a badge,
// a relay, and a server behind a multi-minute idf.py build.
//
//   make && ./blindbox-test

#include "opened_boxes.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_fail = 0;
static int g_pass = 0;

static void ok(bool cond, const char *what)
{
    if (cond) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("FAIL %s\n", what);
}

int main()
{
    OpenedBoxes ob;

    // Fresh set suppresses nothing.
    ok(!ob.suppressed("relay-01"), "fresh set is empty");
    ok(ob.count() == 0, "fresh count is zero");

    // Recording suppresses, forever — no time window to expire.
    ob.record("relay-01");
    ok(ob.suppressed("relay-01"), "recorded box suppressed");
    ok(ob.count() == 1, "count tracks records");

    // Other boxes are unaffected, and re-recording is a no-op.
    ok(!ob.suppressed("relay-02"), "other box fires");
    ob.record("relay-01");
    ok(ob.count() == 1, "re-record adds no slot");

    // Empty ids never suppress and never record.
    ok(!ob.suppressed(""), "empty id not suppressed");
    ok(!ob.suppressed(nullptr), "null id not suppressed");
    ob.record("");
    ob.record(nullptr);
    ok(ob.count() == 1, "empty record is a no-op");

    // Persistence round-trip: serialize, parse into a fresh set (as a
    // reboot would), suppression survives.
    char joined[OpenedBoxes::kJoinCap] = {};
    ob.serialize(joined, sizeof(joined));
    ok(strcmp(joined, "relay-01") == 0, "single id serializes bare");
    OpenedBoxes reloaded;
    reloaded.parse(joined);
    ok(reloaded.suppressed("relay-01"), "suppression survives reload");
    ok(!reloaded.suppressed("relay-02"), "others still fire after reload");
    ok(reloaded.count() == 1, "count survives reload");

    // Multi-id join/parse with stray whitespace.
    OpenedBoxes multi;
    multi.record("a");
    multi.record("b");
    multi.record("c");
    char mjoined[OpenedBoxes::kJoinCap] = {};
    multi.serialize(mjoined, sizeof(mjoined));
    OpenedBoxes mparsed;
    mparsed.parse("  a  b c  ");
    ok(mparsed.suppressed("a") && mparsed.suppressed("b") &&
           mparsed.suppressed("c"),
       "all ids survive join/parse");
    ok(mparsed.count() == 3, "parse skips stray spaces");
    OpenedBoxes empty;
    empty.parse("");
    ok(empty.count() == 0, "empty string parses to empty set");
    empty.parse(nullptr);
    ok(empty.count() == 0, "null parses to empty set");

    // Capacity: the 25th distinct box stays scannable rather than evicting
    // an opened one into rescannability.
    OpenedBoxes full;
    char id[16];
    for (int i = 0; i < OpenedBoxes::kCap; i++) {
        snprintf(id, sizeof(id), "box-%d", i);
        ok(full.record(id), "record within capacity");
    }
    ok(full.count() == OpenedBoxes::kCap, "ring fills to capacity");
    ok(!full.record("box-extra"), "record past capacity fails");
    ok(!full.suppressed("box-extra"), "overflow box stays scannable");
    ok(full.suppressed("box-0"), "nothing evicted by overflow");

    // Worst case still fits one hal_config line: 24 full-length ids.
    OpenedBoxes worst;
    std::string big(37, 'x');
    for (int i = 0; i < OpenedBoxes::kCap; i++) {
        char wid[40];
        snprintf(wid, sizeof(wid), "%s%02d", big.c_str(), i);
        if (!worst.record(wid)) {
            ok(false, "worst-case record fits");
            break;
        }
    }
    char wjoined[OpenedBoxes::kJoinCap] = {};
    worst.serialize(wjoined, sizeof(wjoined));
    ok(strlen(wjoined) < 1151, "worst case fits the config line");
    OpenedBoxes wparsed;
    wparsed.parse(wjoined);
    ok(wparsed.count() == OpenedBoxes::kCap, "worst case reloads whole");

    // Replayable booth: the final box never joins the once-ever set, so it
    // can be opened on every visit. The firmware checks this before
    // suppressed()/record().
    ok(OpenedBoxes::replayable("solana-booth-final"), "final booth replayable");
    ok(!OpenedBoxes::replayable("relay-01"), "normal box not replayable");
    ok(!OpenedBoxes::replayable(""), "empty id not replayable");
    ok(!OpenedBoxes::replayable(nullptr), "null id not replayable");

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

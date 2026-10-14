#!/usr/bin/env python3
"""Exercise real badge/station transfer engines with lossy advertising registers."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="badge-radio-test-") as directory:
    out = Path(directory)
    headers = {
        "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n",
        "esp_log.h": "#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n",
        "esp_timer.h": '#pragma once\n#include <stdint.h>\n#ifdef __cplusplus\nextern "C" {\n#endif\nint64_t esp_timer_get_time(void);\n#ifdef __cplusplus\n}\n#endif\n',
        "esp_rom_crc.h": '#pragma once\n#include <stdint.h>\n#ifdef __cplusplus\nextern "C" {\n#endif\nuint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t *data, uint32_t len);\n#ifdef __cplusplus\n}\n#endif\n',
        "esp_random.h": '#pragma once\n#include <stdint.h>\ninline uint32_t esp_random() { static uint32_t n = 10; return ++n; }\n',
        "freertos/FreeRTOS.h": '#pragma once\n#define pdTRUE 1\n#define portMAX_DELAY 0\n',
        "freertos/task.h": '#pragma once\n',
        "freertos/semphr.h": '#pragma once\ntypedef int SemaphoreHandle_t;\nstatic inline int xSemaphoreCreateMutex(void) { return 1; }\nstatic inline int xSemaphoreTake(int a, int b) { return 1; }\nstatic inline void xSemaphoreGive(int a) {}\n',
    }
    for name, content in headers.items():
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    (out / "xfer_rx.h").write_text((root / "tools/sync_station/main/xfer_rx.h").read_text().replace('#include "bbx_proto.h"', ""))
    source = r'''#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "main/apps/bbx_xfer.h"
#include "main/hal/hal_radio.h"
extern "C" {
#include "xfer_rx.h"
}
static int64_t clock_us;
static std::vector<uint8_t> badge_adv, station_adv;
static bool advertising = true;
extern "C" int64_t esp_timer_get_time() { return clock_us; }
extern "C" uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t *data, uint32_t len) {
    uint32_t crc = ~seed;
    for (uint32_t i = 0; i < len; ++i) { crc ^= data[i]; for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1)); }
    return ~crc;
}
esp_err_t hal_radio_send(const void *p, size_t n) { badge_adv.assign((const uint8_t *)p, (const uint8_t *)p+n); return 0; }
esp_err_t hal_radio_adv_pause() { advertising = false; return 0; }
esp_err_t hal_radio_adv_resume() { advertising = true; return 0; }
esp_err_t hal_radio_set_adv_interval_ms(uint32_t, uint32_t) { return 0; }
esp_err_t hal_radio_set_scan_duty_ms(uint32_t, uint32_t) { return 0; }
void hal_radio_reset_timings() {}
void hal_radio_local_mac(uint8_t out[6]) { memset(out, 0x12, 6); }
extern "C" esp_err_t ble_link_set_payload(const void *p, size_t n) { station_adv.assign((const uint8_t *)p, (const uint8_t *)p+n); return 0; }
extern "C" esp_err_t ble_link_set_adv_interval_ms(uint32_t, uint32_t) { return 0; }
int main() {
    const std::string snapshot(8192, 's');
    const std::string profile = "{\"display_name\":\"New name\",\"padding\":\"" + std::string(1500, 'p') + "\"}";
    // RAM and file-backed transfers, dropped/repeated frames, corrupt CRC,
    // missing return, and invalid size.
    for (int scenario = 0; scenario < 7; ++scenario) {
        bbx_xfer_abort(); xfer_rx_init(); badge_adv.clear(); station_adv.clear();
        const bool lossy = scenario == 1 || scenario == 6;
        FILE *file = nullptr;
        uint8_t response[BBX_PROFILE_MAX_LEN + BBX_PROFILE_PREFIX_LEN + 1] = {};
        XferConfig cfg = {};
        cfg.data = (const uint8_t *)snapshot.data(); cfg.len = snapshot.size();
        cfg.kind = BBX_XFER_KIND_UPLOAD; cfg.enc = BBX_XFER_ENC_RAW;
        cfg.response = response; cfg.response_cap = sizeof(response);
        if (scenario >= 5) {
            file = tmpfile(); assert(file);
            assert(fwrite(snapshot.data(), 1, snapshot.size(), file) == snapshot.size());
            fflush(file);
            // Leave the cursor at EOF: each DATA read must seek to its frame,
            // including a repair that revisits an earlier frame.
            cfg.file = file;
            cfg.crc = esp_rom_crc32_le(0, cfg.data, cfg.len);
            assert(!bbx_xfer_start(cfg)); // ambiguous RAM + file source
            cfg.data = nullptr;
        }
        assert(bbx_xfer_start(cfg));
        int requests = 0, done_at = -1;
        bool first_sweep = true;
        for (int tick = 0; tick < 4000; ++tick) {
            clock_us += 20000;
            if (advertising && !badge_adv.empty() && (!lossy || tick % 3)) {
                auto packet = badge_adv;
                // First deliver unrelated stale ACKs: they must not advance the station.
                auto *h = (bbx_hdr_t *)packet.data();
                if (lossy && h->type == BBX_TYPE_PROFILE_ACK) {
                    ++h->req_id; xfer_rx_on_frame(packet.data(), packet.size());
                }
                // Lose a whole DATA dwell, so recovery must revisit the file
                // after reaching EOF instead of succeeding on a repeat.
                if (!(lossy && first_sweep && h->type == BBX_TYPE_DATA && h->seq == 3))
                    xfer_rx_on_frame(badge_adv.data(), badge_adv.size());
                if (h->type == BBX_TYPE_XFER_OPEN && (h->flags & BBX_FLAG_SWEEP_DONE))
                    first_sweep = false;
            }
            char *body = nullptr; size_t len = 0; uint8_t mac[6], sess, downlink;
            if (xfer_rx_take_upload(&body, &len, mac, &sess, &downlink)) {
                ++requests;
                assert(downlink == 1 && std::string(body, len) == snapshot);
                if (scenario == 3) xfer_rx_finish_upload(BBX_ST_OK);
                else xfer_rx_finish_profile(profile.data(), profile.size(), BBX_ST_OK);
            }
            if (!station_adv.empty() && (!lossy || tick % 4)) {
                auto packet = station_adv; auto *h = (bbx_hdr_t *)packet.data();
                if (h->type == BBX_TYPE_PROFILE_DATA) {
                    if (scenario == 2 && h->seq == 0) packet[BBX_HDR_SIZE + 2] ^= 1;
                    if (scenario == 4 && h->seq == 0) { packet[BBX_HDR_SIZE] = 0xff; packet[BBX_HDR_SIZE+1] = 0xff; }
                    if (lossy && tick % 7 == 0) ++h->req_id;
                }
                bbx_xfer_on_frame(nullptr, -30, packet.data(), packet.size());
            }
            bbx_xfer_tick(); xfer_rx_tick();
            if (bbx_xfer_state() == XferState::Done || bbx_xfer_state() == XferState::Failed) {
                if (done_at < 0) done_at = tick;
                if (tick > done_at + 400) break; // also exercise final ACK and lease cleanup
            }
        }
        assert(requests == 1);
        assert(!advertising); // terminal frames stop after the ACK hold
        if (lossy) assert(bbx_xfer_stats()->repair_rounds >= 1);
        size_t len = 0; const char *returned = bbx_xfer_profile(&len);
        if (scenario < 2 || scenario >= 5) { assert(bbx_xfer_state() == XferState::Done); assert(returned && std::string(returned, len) == profile); }
        else { assert(bbx_xfer_state() == XferState::Failed); assert(!returned); }
        // No badge may retain the station indefinitely after success or failure.
        clock_us += 100000000; xfer_rx_tick(); assert(!xfer_rx_busy());
        bbx_xfer_abort();
        if (file) fclose(file);
    }
    // Cancelling an OPEN must stop the repeating request so it cannot keep
    // re-acquiring the station after its lease expires.
    XferConfig cancelled = {};
    cancelled.data = (const uint8_t *)snapshot.data(); cancelled.len = snapshot.size();
    cancelled.kind = BBX_XFER_KIND_UPLOAD; cancelled.enc = BBX_XFER_ENC_RAW;
    assert(bbx_xfer_start(cancelled));
    xfer_rx_on_frame(badge_adv.data(), badge_adv.size());
    assert(xfer_rx_busy());
    bbx_xfer_abort(); assert(!advertising);
    clock_us += 100000000; xfer_rx_tick();
    clock_us += 100000000; xfer_rx_tick(); assert(!xfer_rx_busy());
}
'''
    (out / "test.cpp").write_text(source)
    include = ["-I", str(out), "-I", str(root), "-I", str(root / "main/hal")]
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", *include,
                    "-DCONFIG_SYNC_MAX_BLOB_BYTES=52785", "-DCONFIG_SYNC_ADV_INTERVAL_MS=30",
                    "-c", str(root / "tools/sync_station/main/xfer_rx.c"), "-o", str(out / "station.o")], check=True)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", *include,
                    str(out / "test.cpp"), str(root / "main/apps/bbx_xfer.cpp"), str(out / "station.o"), "-o", str(out / "test")], check=True)
    subprocess.run([str(out / "test")], check=True)
    print("radio: RAM/file transfer, loss/repetition, CRC, bounds, missing profile, and lease cleanup passed")

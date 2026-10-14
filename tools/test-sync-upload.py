#!/usr/bin/env python3
"""Host checks against the actual NVS credential and station HTTP code (no badge)."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="badge-sync-test-") as folder:
    out = Path(folder)
    headers = {
        "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
inline const char *esp_err_to_name(int) { return "error"; }
""",
        "esp_log.h": """#pragma once
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
""",
        "nvs_flash.h": '#pragma once\ninline int nvs_flash_init() { return 0; }\n',
        "nvs.h": """#pragma once
#include <cstring>
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
static unsigned char persisted[256];
static size_t persisted_len = 0;
static bool write_fails = false;
inline int nvs_open(const char *, int, int *h) { *h = 1; return 0; }
inline int nvs_get_blob(int, const char *, void *p, size_t *n) {
    if (!persisted_len || *n < persisted_len) return -1;
    memcpy(p, persisted, persisted_len); *n = persisted_len; return 0;
}
inline int nvs_set_blob(int, const char *, const void *p, size_t n) {
    if (write_fails) return -1;
    memcpy(persisted, p, n); persisted_len = n; return 0;
}
inline int nvs_commit(int) { return 0; }
inline void nvs_close(int) {}
inline int nvs_erase_key(int, const char *) { persisted_len = 0; return 0; }
""",
        "led_strip.h": """#pragma once
#include <cstring>
#include "esp_err.h"
#define LED_MODEL_WS2812 1
#define LED_STRIP_COLOR_COMPONENT_FMT_GRB 2
#define RMT_CLK_SRC_DEFAULT 1
struct led_strip_config_t { int strip_gpio_num; size_t max_leds; int led_model; int color_component_format; struct { int invert_out; } flags; };
struct led_strip_rmt_config_t { int clk_src; int resolution_hz; int mem_block_symbols; };
typedef int *led_strip_handle_t;
static int strip = 1;
static unsigned pixels[6][3], displayed[6][3];
static int refreshes = 0;
inline int led_strip_new_rmt_device(const led_strip_config_t *, const led_strip_rmt_config_t *, int **out) { *out = &strip; return ESP_OK; }
inline int led_strip_set_pixel(int *, size_t i, unsigned r, unsigned g, unsigned b) { pixels[i][0] = r; pixels[i][1] = g; pixels[i][2] = b; return ESP_OK; }
inline int led_strip_refresh(int *) { memcpy(displayed, pixels, sizeof(pixels)); ++refreshes; return ESP_OK; }
inline int led_strip_clear(int *h) { memset(pixels, 0, sizeof(pixels)); return led_strip_refresh(h); }
""",
        "led_strip_rmt.h": '#pragma once\n#include "led_strip.h"\n',
        "esp_crt_bundle.h": '#pragma once\ninline int esp_crt_bundle_attach(void *) { return 0; }\n',
        "esp_timer.h": '#pragma once\n#include <cstdint>\nstatic int64_t fake_time_us = 0;\ninline int64_t esp_timer_get_time() { return fake_time_us; }\n',
        "esp_http_client.h": """#pragma once
#include <string>
#include <map>
#include <algorithm>
#include "esp_err.h"
#define HTTP_METHOD_POST 1
struct esp_http_client_config_t { const char *url; int method; int timeout_ms; int (*crt_bundle_attach)(void *); };
typedef void *esp_http_client_handle_t;
static std::map<std::string, std::string> headers;
static std::string posted, response_body;
static size_t response_offset = 0;
static bool complete = true;
static int requests = 0, response_code = 200, declared_length = 0;
inline void *esp_http_client_init(const esp_http_client_config_t *cfg) { assert(cfg->method == HTTP_METHOD_POST); response_offset = 0; ++requests; headers.clear(); posted.clear(); return (void *)1; }
inline int esp_http_client_set_header(void *, const char *k, const char *v) { headers[k] = v; return 0; }
inline int esp_http_client_open(void *, int n) { declared_length = n; return 0; }
inline int esp_http_client_write(void *, const char *p, int n) { int sent = std::min(n, 7); posted.append(p, sent); return sent; }
inline int64_t esp_http_client_fetch_headers(void *) { return 0; }
inline int esp_http_client_get_status_code(void *) { return response_code; }
inline int esp_http_client_read(void *, char *p, int n) { size_t count = std::min((size_t)std::min(n, 11), response_body.size() - response_offset); memcpy(p, response_body.data() + response_offset, count); response_offset += count; return count; }
static int fake_errno = 0, fake_tls_err = 0;
inline int esp_http_client_get_errno(void *) { return fake_errno; }
inline int esp_http_client_get_and_clear_last_tls_error(void *, int *c, int *f) { if (c) *c = fake_tls_err; if (f) *f = 0; return 0; }
inline bool esp_http_client_is_complete_data_received(void *) { return complete; }
inline void esp_http_client_close(void *) {}
inline void esp_http_client_cleanup(void *) {}
""",
    }
    for name, content in headers.items():
        (out / name).write_text(content)
    json_dir = Path(os.environ.get("IDF_PATH", str(Path.home() / ".espressif/v5.5.3/esp-idf"))) / "components/json/cJSON"
    if not (json_dir / "cJSON.c").exists():
        raise SystemExit("Activate ESP-IDF first; these tests use its real cJSON parser.")
    subprocess.run([os.environ.get("CC", "cc"), "-c", str(json_dir / "cJSON.c"), "-o", str(out / "cJSON.o")], check=True)
    tests = {
        "identity": r'''#include <cassert>
#include <string>
#define BADGE_IDENTITY_PATH "identity.json"
#include "main/hal/hal_identity.cpp"
int main() {
    char token[65], got[65]; memset(token, 'a', 64); token[64] = 0;
    assert(!hal_identity_get_upload_token(got));
    assert(hal_identity_set_upload_token(token) != ESP_OK);
    s_id.provisioned = true;
    strcpy(s_id.badge_id, "acorn-alpaca-anchor-apple"); strcpy(s_id.claim_id, "123");
    assert(hal_identity_set_upload_token(token) == ESP_OK);
    assert(hal_identity_get_upload_token(got) && strcmp(got, token) == 0);
    // Reboot: identity is loaded again; the credential must come from NVS.
    BadgeIdentity saved = s_id; memset(&s_id, 0, sizeof(s_id)); s_id = saved;
    assert(hal_identity_get_upload_token(got) && strcmp(got, token) == 0);
    strcpy(s_id.claim_id, "124"); assert(!hal_identity_get_upload_token(got));
    s_id = saved; strcpy(s_id.badge_id, "another-badge"); assert(!hal_identity_get_upload_token(got));
    s_id = saved; assert(hal_identity_set_upload_token("short") != ESP_OK);
    token[0] = 'A'; assert(hal_identity_set_upload_token(token) != ESP_OK);
    memset(token, 'b', 64); assert(hal_identity_set_upload_token(token) == ESP_OK);
    assert(hal_identity_get_upload_token(got) && got[0] == 'b');
    write_fails = true; token[0] = 'c'; assert(hal_identity_set_upload_token(token) != ESP_OK);
    assert(hal_identity_get_upload_token(got) && got[0] == 'b'); write_fails = false;
    persisted_len--; assert(!hal_identity_get_upload_token(got)); persisted_len++;
    const char *identity = R"({"version":1,"badge_id":"acorn-alpaca-anchor-apple","claim_id":"123","display_name":"Before","attendee_id":7,"role":"hacker","account_email":"private@example.test","provisioned_unix":100})";
    FILE *f = fopen(BADGE_IDENTITY_PATH, "wb"); assert(f); fputs(identity, f); fclose(f);
    assert(hal_identity_reload() == ESP_OK);
    const std::string profile = R"({"badge_id":"acorn-alpaca-anchor-apple","claim_id":"123","display_name":"After","net_email":"public@example.test","net_phone":"","net_linkedin":"","net_discord":"","net_instagram":"","net_x":""})";
    const uint32_t revision = hal_identity_revision(), sync_revision = hal_identity_sync_revision();
    assert(hal_identity_apply_profile(profile.c_str(), profile.size()) == ESP_OK);
    assert(hal_identity_revision() == revision + 1 && hal_identity_sync_revision() == sync_revision + 1);
    assert(strcmp(s_id.display_name, "After") == 0 && strcmp(s_id.net_email, "public@example.test") == 0);
    assert(strcmp(s_id.account_email, "private@example.test") == 0 && s_id.attendee_id == 7 && s_id.provisioned_unix == 100);
    assert(hal_identity_get_upload_token(got) && got[0] == 'b');
    for (std::string invalid : {std::string("{}"), profile + "junk", profile.substr(0, profile.size() - 1) + R"(,"role":"organizer"})"}) {
        assert(hal_identity_apply_profile(invalid.c_str(), invalid.size()) != ESP_OK);
    }
    for (const std::pair<std::string, std::string> change : {std::pair<std::string, std::string>{"123", "124"}, {"After", std::string(40, 'x')}, {"After", ""}}) {
        std::string invalid = profile; invalid.replace(invalid.find(change.first), change.first.size(), change.second);
        assert(hal_identity_apply_profile(invalid.c_str(), invalid.size()) != ESP_OK);
    }
    assert(hal_identity_revision() == revision + 1 && hal_identity_sync_revision() == sync_revision + 1);
    assert(hal_identity_reload() == ESP_OK && strcmp(s_id.display_name, "After") == 0);
    assert(hal_identity_revision() == revision + 2 && hal_identity_sync_revision() == sync_revision + 1);
    std::string cleared = profile; cleared.replace(cleared.find("public@example.test"), strlen("public@example.test"), "");
    assert(hal_identity_apply_profile(cleared.c_str(), cleared.size()) == ESP_OK && !s_id.net_email[0]);
    assert(hal_identity_get_upload_token(got) && got[0] == 'b');
    hal_identity_erase(); assert(persisted_len == 0 && !hal_identity_get_upload_token(got));
}
''',
        "lights": r'''#include <cassert>
#include "main/hal/hal_lights.cpp"
bool hal_identity_is_provisioned() { return false; }
const BadgeIdentity *hal_identity_get() { static const BadgeIdentity id{}; return &id; }
int main() {
    hal_lights_init();
    // Confirmation pulses must use the same scaled output as a normal app
    // frame, regardless of the HAL's current brightness cap or gamma curve.
    hal_lights_set(0, 0, 255, 0); hal_lights_show();
    const unsigned full_green = displayed[0][1];
    assert(full_green > 0 && full_green < 255);
    for (size_t i = 0; i < LED_COUNT; ++i) hal_lights_set(i, 100, 0, 0);
    hal_lights_show();
    const unsigned red = displayed[0][0]; assert(red > 0 && red < full_green);
    hal_lights_flash_success();
    assert(displayed[0][0] == 0 && displayed[0][1] == full_green);
    int green_pulses = 1;
    const int previous_refreshes = refreshes;
    // An app keeps requesting new colors during the confirmation. It must
    // not interrupt the pulses, and its latest frame must be restored.
    hal_lights_set(0, 0, 0, 255); hal_lights_show(); hal_lights_tick();
    assert(refreshes == previous_refreshes && displayed[0][1] == full_green);
    for (int phase = 1; phase <= 6; ++phase) {
        fake_time_us = phase * 250000;
        hal_lights_tick();
        if (phase < 6) {
            assert(displayed[0][0] == 0 && displayed[0][2] == 0);
            assert(displayed[0][1] == (phase % 2 == 0 ? full_green : 0u));
            if (phase % 2 == 0) ++green_pulses;
        }
    }
    assert(green_pulses == 3 && displayed[0][2] == full_green && displayed[1][0] == red);
    const int finished_refreshes = refreshes;
    fake_time_us += 500000; hal_lights_tick();
    assert(refreshes == finished_refreshes);
    hal_lights_flash_success(); hal_lights_clear(); hal_lights_show();
    fake_time_us += 1500000; hal_lights_tick();
    for (size_t i = 0; i < LED_COUNT; ++i)
        for (unsigned value : displayed[i]) assert(value == 0);
}
''',
        "station": r'''#include <cassert>
#include <string>
#define CONFIG_SYNC_HTTP_TIMEOUT_MS 15000
#define CONFIG_SYNC_AUTH_TOKEN "local-test-\"password\\quoted"
#define CONFIG_SYNC_STATION_ID "test-station"
#include "tools/sync_station/main/net_post.c"
bool wifi_sta_connected() { return true; }
int main() {
    const std::string token(64, 'a');
    const std::string snapshot = "{\"files\":{\"/identity.json\":{\"text\":\"safe\"}}}";
    const std::string envelope = "HTNSYNC1\n123\n" + token + "\n" + snapshot;
    net_post_status_t status;
    net_post_upload("http://localhost", envelope.data(), envelope.size(), "aabbcc", &status);
    assert(status.status == BBX_ST_OK && posted.size() == (size_t)declared_length);
    assert(status.transport_err[0] == '\0');
    assert(headers.size() == 1 && headers["Content-Type"] == "application/json");
    cJSON *request = cJSON_Parse(posted.c_str()); assert(request);
    assert(std::string(cJSON_GetObjectItemCaseSensitive(request, "claim_id")->valuestring) == "123");
    assert(std::string(cJSON_GetObjectItemCaseSensitive(request, "badge_secure_token")->valuestring) == token);
    assert(std::string(cJSON_GetObjectItemCaseSensitive(request, "password")->valuestring) == CONFIG_SYNC_AUTH_TOKEN);
    char *saved = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(request, "badge_data"));
    assert(saved && std::string(saved) == snapshot);
    free(saved); cJSON_Delete(request);
    assert(posted.find("HTNSYNC1") == std::string::npos);
    const std::string expected_request = posted;
    char profile[BBX_PROFILE_MAX_LEN + 128];
    response_body = R"({"status":"success","profile":{"badge_id":"test","claim_id":"123","display_name":"Updated"}})";
    const int before_sync = requests;
    net_sync_upload("http://localhost", envelope.data(), envelope.size(), "", profile, sizeof(profile), &status);
    assert(requests == before_sync + 1 && posted == expected_request && status.status == BBX_ST_OK);
    assert(headers.size() == 1 && std::string(profile) == R"({"badge_id":"test","claim_id":"123","display_name":"Updated"})");
    assert(status.profile_len == strlen(profile));
    for (const std::string invalid : {std::string(R"({"status":"success"})"), std::string(R"({"status":"error","profile":{}})"), std::string(R"({"status":"success","profile":{}}junk)"), std::string("bad json"), std::string(4096, 'x')}) {
        response_body = invalid;
        net_sync_upload("http://localhost", envelope.data(), envelope.size(), "", profile, sizeof(profile), &status);
        assert(status.status != BBX_ST_OK);
    }
    response_body = R"({"status":"success","profile":{}})"; complete = false;
    net_sync_upload("http://localhost", envelope.data(), envelope.size(), "", profile, sizeof(profile), &status);
    assert(status.status == BBX_ST_TRANSPORT); complete = true;
    assert(std::string(status.transport_err) == "incomplete");
    // Simulated network errors surface as short screen-sized detail.
    complete = false; fake_errno = 104;
    net_sync_upload("http://localhost", envelope.data(), envelope.size(), "", profile, sizeof(profile), &status);
    assert(status.status == BBX_ST_TRANSPORT && std::string(status.transport_err) == "errno 104");
    fake_errno = 0; fake_tls_err = 0x7f00;
    net_sync_upload("http://localhost", envelope.data(), envelope.size(), "", profile, sizeof(profile), &status);
    assert(status.status == BBX_ST_TRANSPORT && std::string(status.transport_err) == "tls 0x7f00");
    fake_tls_err = 0; complete = true;
    const int initial_requests = requests;
    for (const std::string &bad : {snapshot, std::string("HTNSYNC1\n0\n") + token + "\n{}", std::string("HTNSYNC1\n123\nshort\n{}"), std::string("HTNSYNC1\n123\n") + token + "\n[]", std::string("HTNSYNC1\n123\n") + std::string(64, 'A') + "\n{}", envelope + ",\"claim_id\":\"999\"", envelope + "junk"}) {
        net_post_upload("http://localhost", bad.data(), bad.size(), "", &status);
        assert(status.status != BBX_ST_OK && requests == initial_requests);
    }
    for (int code : {201, 302, 400, 401, 403, 413, 503}) {
        response_code = code;
        net_post_upload("http://localhost", envelope.data(), envelope.size(), "", &status);
        assert(status.status == BBX_ST_HTTP && status.http_code == code);
        net_sync_upload("http://localhost", envelope.data(), envelope.size(), "", profile, sizeof(profile), &status);
        assert(status.status == BBX_ST_HTTP && status.http_code == code);
    }
}
''',
    }
    for name, source in tests.items():
        path = out / (name + ".cpp")
        path.write_text(source)
        exe = out / name
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-I", str(json_dir), "-I", str(out), "-I", str(ROOT), "-I", str(ROOT / "main"), "-I", str(ROOT / "main/hal"), str(path), str(out / "cJSON.o"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, cwd=out)
        print(name + ": passed")

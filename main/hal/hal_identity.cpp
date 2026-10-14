#include "hal_identity.h"
#include "lanyard_palette.h"

#include "esp_log.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <strings.h>    // strcasecmp
#include <unistd.h>     // unlink
#include <sys/stat.h>   // stat

static const char *TAG = "hal_identity";

// identity.json is written by the registration tool. Cap the read so a
// corrupt or hostile file can't exhaust the heap; a full record with every
// field at max width serialises to well under 1 KB.
static constexpr size_t kMaxFileBytes = 4096;

static BadgeIdentity s_id = {};
static std::atomic<uint32_t> s_revision{0};
static std::atomic<uint32_t> s_sync_revision{0};

static const char *kRoleNames[ROLE_COUNT] = {
    "Hacker", "Organizer", "Sponsor", "Judge",
    "Mentor", "Volunteer", "Media",   "Staff",
    "General", "Workshop Lead", "Visitor",
};

// Canonical registration keys. In particular, workshop_lead is deliberately
// underscored to match the registration source data and HTML option value.
static const char *kRoleKeys[ROLE_COUNT] = {
    "hacker", "organizer", "sponsor", "judge",
    "mentor", "volunteer", "media", "staff",
    "general", "workshop_lead", "visitor",
};

// Badge roles that appear in the Swag reference use exactly the same palette.
// Media and Staff are not listed there, so retain their display fallbacks.
static const std::uint32_t kRoleColors[ROLE_COUNT] = {
    lanyard_palette::kHacker,
    lanyard_palette::kOrganizer,
    lanyard_palette::kSponsor,
    lanyard_palette::kJudge,
    lanyard_palette::kMentor,
    lanyard_palette::kVolunteer,
    0xe84d8a, // Media
    0x95a5a6, // Staff
    lanyard_palette::kGeneral,
    lanyard_palette::kMentor, // Workshop leads share the orange mentor lanyard
    lanyard_palette::kVisitor,
};

const char *hal_identity_role_name(uint8_t role)
{
    if (role >= ROLE_COUNT) return "Attendee";
    return kRoleNames[role];
}

void hal_identity_role_default_color(uint8_t role, uint8_t out_rgb[3])
{
    if (!out_rgb) return;
    const std::uint32_t color =
        (role < ROLE_COUNT) ? kRoleColors[role] : kRoleColors[ROLE_HACKER];
    out_rgb[0] = (uint8_t)((color >> 16) & 0xff);
    out_rgb[1] = (uint8_t)((color >> 8) & 0xff);
    out_rgb[2] = (uint8_t)(color & 0xff);
}

// Copy a JSON string field into a fixed buffer. Returns false if the value is
// present but too long — the caller rejects the whole record rather than
// silently truncating, so a bad provision fails loudly at the desk instead of
// quietly at the event.
static bool copy_str(const cJSON *root, const char *key, char *out, size_t out_sz)
{
    out[0] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item || cJSON_IsNull(item)) return true;      // absent is fine
    if (!cJSON_IsString(item) || !item->valuestring) {
        ESP_LOGE(TAG, "%s: not a string", key);
        return false;
    }
    size_t len = strlen(item->valuestring);
    if (len >= out_sz) {
        ESP_LOGE(TAG, "%s: %u bytes, max %u", key,
                 (unsigned)len, (unsigned)(out_sz - 1));
        return false;
    }
    memcpy(out, item->valuestring, len + 1);
    return true;
}

static uint32_t get_u32(const cJSON *root, const char *key, uint32_t def)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) return def;
    if (cJSON_IsNumber(item)) return (uint32_t)item->valuedouble;
    // The registration tool may send numeric ids as strings; accept both so a
    // JSON-type mismatch doesn't anonymise a badge.
    if (cJSON_IsString(item) && item->valuestring) {
        return (uint32_t)strtoul(item->valuestring, nullptr, 10);
    }
    return def;
}

// role accepts either the enum ordinal or the lowercase name.
static uint8_t parse_role(const cJSON *root)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "role");
    if (!item) return ROLE_HACKER;
    if (cJSON_IsNumber(item)) {
        int v = (int)item->valuedouble;
        return (v >= 0 && v < ROLE_COUNT) ? (uint8_t)v : (uint8_t)ROLE_HACKER;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        for (int i = 0; i < ROLE_COUNT; i++) {
            if (strcasecmp(item->valuestring, kRoleNames[i]) == 0
                || strcasecmp(item->valuestring, kRoleKeys[i]) == 0) {
                return (uint8_t)i;
            }
        }
        ESP_LOGW(TAG, "unknown role '%s', defaulting to hacker", item->valuestring);
    }
    return ROLE_HACKER;
}

// "#RRGGBB" or [r,g,b]. Falls back to the role default.
static void parse_role_color(const cJSON *root, uint8_t role, uint8_t out[3])
{
    hal_identity_role_default_color(role, out);

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "role_color");
    if (!item) return;

    if (cJSON_IsString(item) && item->valuestring) {
        const char *s = item->valuestring;
        if (*s == '#') s++;
        if (strlen(s) == 6) {
            char buf[3] = {};
            for (int i = 0; i < 3; i++) {
                buf[0] = s[i * 2];
                buf[1] = s[i * 2 + 1];
                out[i] = (uint8_t)strtoul(buf, nullptr, 16);
            }
        }
        return;
    }
    if (cJSON_IsArray(item) && cJSON_GetArraySize(item) == 3) {
        for (int i = 0; i < 3; i++) {
            const cJSON *c = cJSON_GetArrayItem(item, i);
            if (cJSON_IsNumber(c)) {
                int v = (int)c->valuedouble;
                out[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
        }
    }
}

static char *read_file(const char *path, size_t *out_len)
{
    struct stat st;
    if (stat(path, &st) != 0) return nullptr;
    if (st.st_size <= 0 || (size_t)st.st_size > kMaxFileBytes) {
        ESP_LOGE(TAG, "identity.json size %ld out of range", (long)st.st_size);
        return nullptr;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;

    size_t len = (size_t)st.st_size;
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        fclose(f);
        return nullptr;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static bool parse_into(BadgeIdentity *dst, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "identity.json is not valid JSON");
        return false;
    }

    bool ok = true;
    BadgeIdentity tmp = {};
    tmp.version = (uint16_t)get_u32(root, "version", 1);

    ok = ok && copy_str(root, "badge_id",       tmp.badge_id,       sizeof(tmp.badge_id));
    ok = ok && copy_str(root, "display_name",   tmp.display_name,   sizeof(tmp.display_name));
    ok = ok && copy_str(root, "account_email",  tmp.account_email,  sizeof(tmp.account_email));
    ok = ok && copy_str(root, "claim_id",       tmp.claim_id,       sizeof(tmp.claim_id));
    ok = ok && copy_str(root, "net_email",      tmp.net_email,      sizeof(tmp.net_email));
    ok = ok && copy_str(root, "net_phone",      tmp.net_phone,      sizeof(tmp.net_phone));
    ok = ok && copy_str(root, "net_linkedin",   tmp.net_linkedin,   sizeof(tmp.net_linkedin));
    ok = ok && copy_str(root, "net_discord",    tmp.net_discord,    sizeof(tmp.net_discord));
    ok = ok && copy_str(root, "net_instagram",  tmp.net_instagram,  sizeof(tmp.net_instagram));
    ok = ok && copy_str(root, "net_x",          tmp.net_x,          sizeof(tmp.net_x));

    tmp.attendee_id      = get_u32(root, "attendee_id", 0);
    tmp.provisioned_unix = get_u32(root, "provisioned_unix", 0);
    tmp.role             = parse_role(root);
    parse_role_color(root, tmp.role, tmp.role_color);

    // A record with no badge_id or no name is not usable. Better to show the
    // "see registration" screen than a nameless badge.
    if (ok && tmp.badge_id[0] == '\0') {
        ESP_LOGE(TAG, "badge_id is missing or empty");
        ok = false;
    }
    if (ok && tmp.display_name[0] == '\0') {
        ESP_LOGE(TAG, "display_name is missing or empty");
        ok = false;
    }

    cJSON_Delete(root);
    if (!ok) return false;

    tmp.provisioned = true;
    *dst = tmp;
    return true;
}

esp_err_t hal_identity_reload()
{
    memset(&s_id, 0, sizeof(s_id));

    size_t len = 0;
    char *json = read_file(BADGE_IDENTITY_PATH, &len);
    if (!json) {
        ESP_LOGW(TAG, "no identity at %s — badge is unprovisioned",
                 BADGE_IDENTITY_PATH);
        ++s_revision;
        return ESP_ERR_INVALID_ARG;
    }

    bool ok = parse_into(&s_id, json);
    free(json);

    if (!ok) {
        // Leave the record zeroed. Never half-load: a badge showing someone
        // else's name is worse than one showing none.
        memset(&s_id, 0, sizeof(s_id));
        ++s_revision;
        return ESP_ERR_INVALID_ARG;
    }

    ++s_revision;
    ESP_LOGI(TAG, "identity: %s (%s) id=%s",
             s_id.display_name, hal_identity_role_name(s_id.role), s_id.badge_id);
    return ESP_OK;
}

esp_err_t hal_identity_apply_profile(const char *json, size_t len)
{
    if (!s_id.provisioned || !json || len == 0 || len > 2048 || strlen(json) != len)
        return ESP_ERR_INVALID_ARG;
    const char *end = nullptr;
    cJSON *profile = cJSON_ParseWithLengthOpts(json, len + 1, &end, true);
    if (!cJSON_IsObject(profile)) { cJSON_Delete(profile); return ESP_ERR_INVALID_ARG; }
    bool valid = cJSON_GetArraySize(profile) == 9;
    const char *keys[] = {"badge_id", "claim_id", "display_name", "net_email", "net_phone",
                          "net_linkedin", "net_discord", "net_instagram", "net_x"};
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, profile) {
        bool known = false;
        for (const char *key : keys) if (item->string && strcmp(item->string, key) == 0) known = true;
        if (!known || !cJSON_IsString(item) || !item->valuestring) valid = false;
    }
    for (const char *key : keys) {
        item = cJSON_GetObjectItemCaseSensitive(profile, key);
        if (!cJSON_IsString(item) || !item->valuestring) valid = false;
    }
    if (valid && (strcmp(cJSON_GetObjectItemCaseSensitive(profile, "badge_id")->valuestring, s_id.badge_id) != 0
        || strcmp(cJSON_GetObjectItemCaseSensitive(profile, "claim_id")->valuestring, s_id.claim_id) != 0)) valid = false;
    size_t old_len = 0;
    char *old_json = valid ? read_file(BADGE_IDENTITY_PATH, &old_len) : nullptr;
    cJSON *identity = old_json ? cJSON_Parse(old_json) : nullptr;
    free(old_json);
    if (!cJSON_IsObject(identity)) valid = false;
    if (valid) {
        for (size_t i = 2; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            cJSON_DeleteItemFromObjectCaseSensitive(identity, keys[i]);
            if (!cJSON_AddStringToObject(identity, keys[i], cJSON_GetObjectItemCaseSensitive(profile, keys[i])->valuestring)) valid = false;
        }
    }
    char *updated = valid ? cJSON_PrintUnformatted(identity) : nullptr;
    BadgeIdentity parsed = {};
    valid = updated && strlen(updated) <= kMaxFileBytes && parse_into(&parsed, updated)
        && strcmp(parsed.badge_id, s_id.badge_id) == 0 && strcmp(parsed.claim_id, s_id.claim_id) == 0
        && parsed.attendee_id == s_id.attendee_id && parsed.role == s_id.role;
    cJSON_Delete(profile);
    cJSON_Delete(identity);
    if (!valid) { free(updated); return ESP_ERR_INVALID_ARG; }

    // LittleFS rename replaces the old file atomically. A failed write leaves
    // the old identity, in-memory record and NVS credential intact.
    const char *tmp = BADGE_IDENTITY_PATH ".sync.tmp";
    FILE *file = fopen(tmp, "wb");
    bool written = false;
    if (file) {
        const size_t bytes = strlen(updated);
        written = fwrite(updated, 1, bytes, file) == bytes && fflush(file) == 0;
        if (fclose(file) != 0) written = false;
    }
    free(updated);
    if (!written || rename(tmp, BADGE_IDENTITY_PATH) != 0) {
        unlink(tmp);
        return ESP_FAIL;
    }
    s_id = parsed;
    ++s_revision;
    ++s_sync_revision; // UI task observes success only after the atomic file replacement.
    return ESP_OK;
}

esp_err_t hal_identity_init()
{
    if (nvs_flash_init() != ESP_OK) return ESP_FAIL;
    hal_identity_reload();   // unprovisioned is a valid state, not a boot failure
    return ESP_OK;
}

bool hal_identity_is_provisioned()
{
    return s_id.provisioned;
}

const BadgeIdentity *hal_identity_get()
{
    return &s_id;
}

void hal_identity_erase()
{
    nvs_handle_t handle;
    if (nvs_open("badge_upload", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_key(handle, "credential");
        nvs_commit(handle);
        nvs_close(handle);
    }
    unlink(BADGE_IDENTITY_PATH);
    memset(&s_id, 0, sizeof(s_id));
    ++s_revision;
    ESP_LOGW(TAG, "identity erased");
}

namespace {
struct UploadCredential {
    char token[65];
    char badge_id[64];
    char claim_id[24];
};
}

bool hal_identity_get_upload_token(char out[65])
{
    out[0] = 0;
    if (!s_id.provisioned || !s_id.claim_id[0]) return false;
    nvs_handle_t handle;
    if (nvs_open("badge_upload", NVS_READONLY, &handle) != ESP_OK) return false;
    UploadCredential credential = {};
    size_t len = sizeof(credential);
    esp_err_t err = nvs_get_blob(handle, "credential", &credential, &len);
    nvs_close(handle);
    bool valid = err == ESP_OK && len == sizeof(credential)
        && credential.token[64] == 0
        && memcmp(credential.badge_id, s_id.badge_id, sizeof(s_id.badge_id)) == 0
        && memcmp(credential.claim_id, s_id.claim_id, sizeof(s_id.claim_id)) == 0;
    for (size_t i = 0; valid && i < 64; ++i) {
        char c = credential.token[i];
        valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    }
    if (valid) memcpy(out, credential.token, 65);
    memset(&credential, 0, sizeof(credential));
    return valid;
}

esp_err_t hal_identity_set_upload_token(const char *token)
{
    if (!s_id.provisioned || !s_id.claim_id[0] || !token || strlen(token) != 64)
        return ESP_ERR_INVALID_ARG;
    for (const char *p = s_id.claim_id; *p; ++p)
        if (*p < '0' || *p > '9') return ESP_ERR_INVALID_ARG;
    if (s_id.claim_id[0] == '0') return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < 64; ++i)
        if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f')))
            return ESP_ERR_INVALID_ARG;
    UploadCredential credential = {};
    memcpy(credential.token, token, 65);
    memcpy(credential.badge_id, s_id.badge_id, sizeof(s_id.badge_id));
    memcpy(credential.claim_id, s_id.claim_id, sizeof(s_id.claim_id));
    nvs_handle_t handle;
    esp_err_t err = nvs_open("badge_upload", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "credential", &credential, sizeof(credential));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    memset(&credential, 0, sizeof(credential));
    return err;
}

uint32_t hal_identity_revision()
{
    return s_revision.load();
}

uint32_t hal_identity_sync_revision()
{
    return s_sync_revision.load();
}

#include "solana_wallet.h"
#include "hal_radio.h"
#include "hal_identity.h"
#include "pal_catalog.h"

#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"

#include "sodium/crypto_sign.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>

static const char *TAG = "solana";

// Root of LittleFS, not under /littlefs/config — factory_reset does an rm -rf
// on that directory and must never destroy an attendee's wallet.
static const char *WALLET_PATH = "/littlefs/solana.json";

static char s_pubkey[SOLANA_PUBKEY_B58_CAP] = {};
static char s_seckey[SOLANA_SECKEY_B58_CAP] = {};
static char s_last_link[192] = {};
static char s_item_ids[SOLANA_MAX_ITEMS][SOLANA_ITEM_ID_CAP] = {};
static size_t s_item_count = 0;
static int s_reported_item_count = 0;
static uint8_t s_equipped_pal = PAL_NONE;

// Bitcoin/Solana alphabet: no 0, O, I or l, so a key read aloud or copied by
// hand doesn't turn into a different key.
static const char kB58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static bool b58_encode(const uint8_t *in, size_t len, char *out, size_t out_sz)
{
    size_t zeros = 0;
    while (zeros < len && in[zeros] == 0) zeros++;

    // log(256)/log(58) is ~1.365; 138/100 is the standard integer bound.
    size_t bufsz = (len - zeros) * 138 / 100 + 1;
    uint8_t buf[128];
    if (bufsz > sizeof(buf)) return false;
    memset(buf, 0, bufsz);

    for (size_t i = zeros; i < len; i++) {
        int carry = in[i];
        for (size_t j = bufsz; j > 0; j--) {
            carry += 256 * buf[j - 1];
            buf[j - 1] = (uint8_t)(carry % 58);
            carry /= 58;
        }
    }

    size_t k = 0;
    while (k < bufsz && buf[k] == 0) k++;

    if (zeros + (bufsz - k) + 1 > out_sz) return false;

    size_t o = 0;
    for (size_t i = 0; i < zeros; i++) out[o++] = '1';
    for (; k < bufsz; k++) out[o++] = kB58[buf[k]];
    out[o] = '\0';
    return true;
}

static void copy_json_str(const cJSON *root, const char *key, char *out, size_t out_sz)
{
    out[0] = '\0';
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(it) && it->valuestring) {
        snprintf(out, out_sz, "%s", it->valuestring);
    }
}

static bool save()
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "public_key", s_pubkey);
    cJSON_AddStringToObject(root, "secret_key", s_seckey);
    cJSON *items = cJSON_AddArrayToObject(root, "all_items");
    if (!items) {
        cJSON_Delete(root);
        return false;
    }
    for (size_t i = 0; i < s_item_count; ++i) {
        cJSON_AddItemToArray(items, cJSON_CreateString(s_item_ids[i]));
    }
    // Keep the old summary field for upload tools that have not migrated yet.
    cJSON_AddNumberToObject(root, "item_count", s_reported_item_count);
    cJSON_AddNumberToObject(root, "equipped_pal", s_equipped_pal);
    cJSON_AddStringToObject(root, "last_link", s_last_link);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", WALLET_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(json);
        ESP_LOGE(TAG, "open %s failed", tmp);
        return false;
    }
    fputs(json, f);
    fclose(f);
    free(json);

    if (rename(tmp, WALLET_PATH) != 0) {
        ESP_LOGE(TAG, "rename to %s failed", WALLET_PATH);
        return false;
    }
    return true;
}

esp_err_t solana_wallet_init()
{
    s_pubkey[0] = s_seckey[0] = s_last_link[0] = '\0';
    memset(s_item_ids, 0, sizeof(s_item_ids));
    s_item_count = 0;
    s_reported_item_count = 0;
    s_equipped_pal = PAL_NONE;

    struct stat st;
    if (stat(WALLET_PATH, &st) != 0 || st.st_size <= 0 || st.st_size > 4096) {
        return ESP_OK;   // no wallet yet is a normal state
    }

    FILE *f = fopen(WALLET_PATH, "rb");
    if (!f) return ESP_OK;

    char *buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "solana.json is corrupt");
        return ESP_ERR_INVALID_ARG;
    }

    copy_json_str(root, "public_key", s_pubkey, sizeof(s_pubkey));
    copy_json_str(root, "secret_key", s_seckey, sizeof(s_seckey));
    copy_json_str(root, "last_link", s_last_link, sizeof(s_last_link));
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "all_items");
    if (cJSON_IsArray(items)) {
        const cJSON *item = nullptr;
        cJSON_ArrayForEach(item, items) {
            if (s_item_count >= SOLANA_MAX_ITEMS) break;
            if (!cJSON_IsString(item) || !item->valuestring || !*item->valuestring) continue;
            snprintf(s_item_ids[s_item_count], SOLANA_ITEM_ID_CAP, "%s",
                     item->valuestring);
            ++s_item_count;
        }
        s_reported_item_count = (int)s_item_count;
    } else {
        // Legacy files only knew the count. Keep progress intact until the
        // next server response replaces it with authoritative item IDs.
        const cJSON *count = cJSON_GetObjectItemCaseSensitive(root, "item_count");
        if (cJSON_IsNumber(count) && count->valuedouble > 0) {
            s_reported_item_count = (int)count->valuedouble;
        }
    }
    const cJSON *equipped = cJSON_GetObjectItemCaseSensitive(root, "equipped_pal");
    if (cJSON_IsNumber(equipped)) {
        int value = (int)equipped->valuedouble;
        if (value >= PAL_NONE && value < PAL_COUNT) {
            s_equipped_pal = (uint8_t)value;
        }
    } else {
        // Early development builds followed the design doc and stored the
        // equipped Solana item ID as a string.
        equipped = cJSON_GetObjectItemCaseSensitive(root, "equipped");
        if (cJSON_IsString(equipped) && equipped->valuestring) {
            const PalDefinition *pal = pal_catalog_find_item(equipped->valuestring);
            if (pal) s_equipped_pal = pal->id;
        }
    }
    cJSON_Delete(root);

    if (!solana_wallet_pal_unlocked(s_equipped_pal)) s_equipped_pal = PAL_NONE;

    if (s_pubkey[0]) ESP_LOGI(TAG, "wallet %s (%d items)", s_pubkey,
                             s_reported_item_count);
    return ESP_OK;
}

bool solana_wallet_has_keypair()
{
    return s_pubkey[0] != '\0';
}

esp_err_t solana_wallet_ensure_keypair()
{
    if (solana_wallet_has_keypair()) return ESP_OK;

    // Hard entropy precondition. esp_random() only returns true random
    // numbers while the RF subsystem is running; without this check a
    // thousand badges would generate keys from a weak source at the same
    // point in their first boot.
    // hal_radio_enable() is idempotent, so this is cheap when the radio is
    // already up and correct when it isn't.
    if (hal_radio_enable() != ESP_OK) {
        ESP_LOGE(TAG, "refusing to generate a keypair without the radio up");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t seed[crypto_sign_SEEDBYTES];
    uint8_t pk[crypto_sign_PUBLICKEYBYTES];
    uint8_t sk[crypto_sign_SECRETKEYBYTES];
    esp_fill_random(seed, sizeof(seed));

    if (crypto_sign_seed_keypair(pk, sk, seed) != 0) {
        ESP_LOGE(TAG, "crypto_sign_seed_keypair failed");
        return ESP_FAIL;
    }
    memset(seed, 0, sizeof(seed));

    bool ok = b58_encode(pk, sizeof(pk), s_pubkey, sizeof(s_pubkey))
           && b58_encode(sk, sizeof(sk), s_seckey, sizeof(s_seckey));
    memset(sk, 0, sizeof(sk));
    if (!ok) {
        s_pubkey[0] = s_seckey[0] = '\0';
        ESP_LOGE(TAG, "base58 encode failed");
        return ESP_FAIL;
    }

    if (!save()) {
        // Keep the in-memory keypair so the current session still works, but
        // say so — this wallet will not survive a reboot.
        ESP_LOGE(TAG, "generated a keypair but could not persist it");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "generated wallet %s", s_pubkey);
    return ESP_OK;
}

const char *solana_public_key()
{
    return s_pubkey;
}

void solana_wallet_replace_items(const char *const *item_ids, size_t count)
{
    char replacement[SOLANA_MAX_ITEMS][SOLANA_ITEM_ID_CAP] = {};
    size_t replacement_count = 0;
    for (size_t i = 0; i < count && replacement_count < SOLANA_MAX_ITEMS; ++i) {
        if (!item_ids[i] || !*item_ids[i]) continue;
        bool duplicate = false;
        for (size_t j = 0; j < replacement_count; ++j) {
            if (strcmp(replacement[j], item_ids[i]) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        snprintf(replacement[replacement_count], SOLANA_ITEM_ID_CAP, "%s",
                 item_ids[i]);
        ++replacement_count;
    }

    bool changed = replacement_count != s_item_count
        || memcmp(replacement, s_item_ids, sizeof(replacement)) != 0;
    if (!changed) return;

    memcpy(s_item_ids, replacement, sizeof(s_item_ids));
    s_item_count = replacement_count;
    s_reported_item_count = (int)replacement_count;
    if (!solana_wallet_pal_unlocked(s_equipped_pal)) s_equipped_pal = PAL_NONE;
    save();
}

int solana_wallet_item_count()
{
    return s_reported_item_count;
}

bool solana_wallet_pal_unlocked(uint8_t pal_id)
{
    if (pal_id == PAL_NONE) return true;
    const PalDefinition *pal = pal_catalog_get(pal_id);
    if (!pal) return false;

    const BadgeIdentity *identity = hal_identity_get();
    if (identity->provisioned && identity->role == ROLE_ORGANIZER) return true;

    for (size_t i = 0; i < s_item_count; ++i) {
        if (strcmp(s_item_ids[i], pal->item_id) == 0) return true;
    }
    return false;
}

uint8_t solana_wallet_equipped_pal()
{
    return solana_wallet_pal_unlocked(s_equipped_pal)
        ? s_equipped_pal : (uint8_t)PAL_NONE;
}

bool solana_wallet_set_equipped_pal(uint8_t pal_id)
{
    if (!solana_wallet_pal_unlocked(pal_id)) return false;
    if (pal_id == s_equipped_pal) return true;
    const uint8_t previous = s_equipped_pal;
    s_equipped_pal = pal_id;
    if (save()) return true;
    s_equipped_pal = previous;
    return false;
}

void solana_wallet_set_last_link(const char *url)
{
    if (!url || !*url) return;
    snprintf(s_last_link, sizeof(s_last_link), "%s", url);
    save();
}

const char *solana_wallet_last_link()
{
    return s_last_link;
}

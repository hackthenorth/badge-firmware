#pragma once

#include "esp_err.h"
#include <cstdint>
#include <cstddef>

// Badge identity: who this badge belongs to. Provisioned at registration over
// serial (see hal_console's `prov` commands) and read by the ID page, badge
// bump, the Solana app, and the upload payload.
//
// Stored as JSON at /littlefs/identity.json — deliberately at the LittleFS
// ROOT, not under /littlefs/config, because `factory_reset` does an rm -rf on
// /littlefs/config. Do not move it, and do not widen factory_reset.
//
// Known and accepted loss paths: a filesystem format (hal_fs_init has
// format_if_mount_failed = true) and a full-chip erase. Recovery is
// re-provisioning at the badge desk, or a BLE restore keyed on the badge's
// BLE MAC, which registration records at provision time.

#ifndef BADGE_IDENTITY_PATH
#define BADGE_IDENTITY_PATH "/littlefs/identity.json"
#endif

enum BadgeRole : uint8_t {
    ROLE_HACKER = 0,
    ROLE_ORGANIZER,
    ROLE_SPONSOR,
    ROLE_JUDGE,
    ROLE_MENTOR,
    ROLE_VOLUNTEER,
    // Retain the original numeric values above for already-provisioned badges
    // and bump packets. These legacy roles are still readable, but no longer
    // appear in the 2026 registration dropdown.
    ROLE_MEDIA,
    ROLE_STAFF,
    ROLE_GENERAL,
    ROLE_WORKSHOP_LEAD,
    ROLE_VISITOR,
    ROLE_COUNT,
};

// Field widths are the contract with the registration tool and with the bump
// packet. The RSVP form and the provisioner must reject anything longer —
// truncating here would put a mangled name on someone's badge. Sizes include
// the NUL, so the usable text is one less.
struct BadgeIdentity {
    uint16_t version;               // 1
    bool     provisioned;

    char     badge_id[64];          // opaque, server-generated, non-sequential
    uint32_t attendee_id;           // HTN internal — never broadcast
    uint8_t  role;                  // BadgeRole
    uint8_t  role_color[3];         // RGB; LEDs + UI accent, matches lanyard
    uint32_t provisioned_unix;

    char     display_name[40];
    char     account_email[64];     // for tracking. NOT broadcast.
    char     claim_id[24];          // extended profile claim ID

    // Networking fields — these ARE shared over the air on a bump.
    char     net_email[64];
    char     net_phone[20];
    char     net_linkedin[40];
    char     net_discord[40];
    char     net_instagram[32];
    char     net_x[32];
};

// Parse identity.json into the in-memory record. Call after hal_fs_init() and
// before any app that reads identity. Always returns ESP_OK — a missing or
// malformed file leaves the badge unprovisioned rather than failing boot.
esp_err_t hal_identity_init();

bool hal_identity_is_provisioned();

// Never nullptr. If unprovisioned, returns a zeroed record with
// provisioned == false, so callers can read fields without null checks — but
// must still check provisioned before broadcasting or displaying anything.
const BadgeIdentity *hal_identity_get();

// Re-read identity.json from disk. Used by `prov apply` so a freshly
// provisioned badge takes effect without a reboot. Returns ESP_ERR_INVALID_ARG
// if the file is missing or fails validation, leaving the badge unprovisioned.
esp_err_t hal_identity_reload();

// Delete identity.json and clear the record. Staff only.
void hal_identity_erase();

// Human-readable role name ("Hacker", "Organizer", ...). Never nullptr.
const char *hal_identity_role_name(uint8_t role);

// Default lanyard-matching colour for a role, used when the provisioned
// record carries no explicit role_color.
void hal_identity_role_default_color(uint8_t role, uint8_t out_rgb[3]);

// Upload credential lives in NVS, outside the filesystem mirror. It is bound
// to the current badge and claim IDs so an identity replacement cannot reuse it.
// Raw tokens are exactly 32 random bytes encoded as 64 lowercase hex characters.
bool hal_identity_get_upload_token(char out[65]);
esp_err_t hal_identity_set_upload_token(const char *token);

// Apply a bounded server profile to the already-assigned badge. Only display
// name and networking fields change; IDs, role, app mode and NVS are preserved.
// Validates everything before an atomic identity.json replacement.
esp_err_t hal_identity_apply_profile(const char *json, size_t len);

// Monotonic UI notifications. Revisions publish after the in-memory record is
// ready; sync revision advances only after a profile is successfully persisted.
uint32_t hal_identity_revision();
uint32_t hal_identity_sync_revision();

#pragma once

#include "bump_proto.h"

// Persist a peer's card to /littlefs/config/contacts/<badge_id>.cfg.
//
// Keyed on badge_id rather than the peer's BLE MAC: badge_id is what the
// server understands, so an upload can send contacts as ids plus timestamps
// instead of whole cards. Re-bumping the same person overwrites their entry
// rather than creating a duplicate. When supplied, added is true only after
// successfully creating a new contact; updates and failures leave it false.
bool contacts_store_save(const BumpCard &card, bool *added = nullptr);

// Number of stored contacts, for the progress app. Returns 0 if the directory
// doesn't exist yet.
size_t contacts_store_count();

// Curated read-only view for the Lua contacts API. Only the fields a
// third-party script may see: no email, phone, or social handles.
struct ContactRecord {
    char name[40];
    char badge_id[64];
    uint8_t role;
    long long received_unix;
};

// Reads the index-th contact in sorted filename order (readdir order is
// arbitrary, so indices would otherwise shift between calls). Returns false
// when index is out of range or the entry is unreadable.
bool contacts_store_get(size_t index, ContactRecord *out);

#pragma once

#include "esp_err.h"
#include <cstddef>
#include <cstdint>

// The attendee's Solana wallet, used by the Solana Scavenger Hunt.
//
// NOT production custody: no secure element, no attestation, and the secret
// key is exported in the badge backup so a lost badge can be restored with
// its inventory intact. That is a deliberate, documented tradeoff for an
// event toy.
//
// The badge never signs transactions. Solana's server mints to our public
// key and returns a chain link, so all we need is key generation and base58
// — no signing path at all.
//
// Ed25519 comes from libsodium (espressif/libsodium), not a hand-rolled
// implementation. Field arithmetic is ~1500 lines where a subtle mistake is
// silent and still produces valid-looking keys.

#define SOLANA_PUBKEY_B58_CAP 64
#define SOLANA_SECKEY_B58_CAP 128
#define SOLANA_MAX_ITEMS 16
#define SOLANA_ITEM_ID_CAP 32

// Load the wallet from /littlefs/solana.json if present. Does NOT generate a
// keypair — generation needs good entropy, so it is deferred to
// solana_wallet_ensure_keypair(). Safe to call at boot.
esp_err_t solana_wallet_init();

bool solana_wallet_has_keypair();

// Generate and persist a keypair if we don't already have one.
//
// MUST be called with the radio enabled. esp_random() is only a true RNG
// while Wi-Fi or Bluetooth is up; generating at boot with the radio down
// would draw weak entropy on a thousand badges at the same point in their
// first boot, risking correlated keys. Returns ESP_ERR_INVALID_STATE if the
// entropy precondition isn't met.
esp_err_t solana_wallet_ensure_keypair();

// base58 public key, or "" if no keypair exists yet. Never nullptr.
const char *solana_public_key();

// Inventory, replaced wholesale from the server's authoritative `all_items`.
// Unknown IDs are retained so a firmware frozen before the reward catalog is
// extended does not erase newer server state on its next save.
void solana_wallet_replace_items(const char *const *item_ids, size_t count);
int  solana_wallet_item_count();

// PAL_NONE (0) is always available. Known reward PALs are available only when
// their item ID is in the inventory; organizers are granted all nine at read
// time without writing fake Solana rewards into their wallet.
bool    solana_wallet_pal_unlocked(uint8_t pal_id);
uint8_t solana_wallet_equipped_pal();
bool    solana_wallet_set_equipped_pal(uint8_t pal_id);

// Most recent transaction link, shown as a QR to the chain explorer.
void        solana_wallet_set_last_link(const char *url);
const char *solana_wallet_last_link();

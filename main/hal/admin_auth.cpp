#include "admin_auth.h"

#include <sodium.h>
#include <cstring>

namespace {
constexpr unsigned char kAdminPasswordSha256[crypto_hash_sha256_BYTES] = {
    0xb8, 0x53, 0xb1, 0x0a, 0x3c, 0xc4, 0x91, 0x51, 0x64, 0x38, 0x21, 0xe5, 0x3e, 0xcd, 0xce, 0xd8, 0x85, 0x17, 0xb7, 0xcc, 0x80, 0x3a, 0x33, 0x61, 0xe1, 0x79, 0x90, 0x52, 0x39, 0x85, 0x9c, 0x8a
};
}

bool admin_password_valid(const char *password)
{
    if (!password || !*password) return false;
    unsigned char hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash, reinterpret_cast<const unsigned char *>(password),
                       strlen(password));
    const bool valid = sodium_memcmp(hash, kAdminPasswordSha256, sizeof(hash)) == 0;
    sodium_memzero(hash, sizeof(hash));
    return valid;
}

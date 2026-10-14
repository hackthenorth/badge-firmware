#pragma once

// Shared console guard. Only the SHA-256 verifier is part of the firmware;
// host UIs prefill the matching Admin password from their committed defaults.
bool admin_password_valid(const char *password);

#pragma once

// Paint and reveal the Hack the North x Solana partnership card as the LCD's
// first frame. Call boot_splash_finish() once the boot destination is ready;
// the card is then removed after it has been visible for at least one second.
//
// Reboot boots (straight into an app, or back to the launcher after a
// radio/NFC session) never paint the card: the panel stays dark until the
// destination is ready, then main.cpp reveals it with hal_display_show().
void boot_splash_show();
void boot_splash_finish();

# Production checklist

## Release blocker: remove secret testing shortcuts

- [x] Replace the Badge ID unlock-all code with the Bubble Wrap-only secret.
- [x] Remove the Home+Start and long-Left shortcuts that reveal internal apps.
- [x] Remove the A+Left app-clear shortcut and ten-times A+B onboarding replay.
- [ ] Verify NFC tags can unlock public apps but cannot reveal internal apps.

The QR-only Bubble Wrap sequence is an intentional attendee secret. The
Default, Public, and Public + Diagnostics access tiers, the engineer-flasher
selector, and the `appmode` registration command are permanent provisioning
features. Do not ship attendee firmware until the remaining NFC boundary check
has passed.

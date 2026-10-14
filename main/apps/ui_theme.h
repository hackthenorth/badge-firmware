#pragma once

#include <cstdint>

// Hack the North badge UI palette.
//
// This intentionally mirrors the event website: true black foundations,
// warm off-white type, and violet used as a signal instead of a wash. Keeping
// the palette here prevents individual apps from drifting back toward the old
// blue/cyan "phone UI" treatment.
namespace BadgeTheme {

static constexpr std::uint32_t BACKGROUND    = 0x050505;
static constexpr std::uint32_t PANEL         = 0x0b0c0b;
static constexpr std::uint32_t SURFACE       = 0x171017;
static constexpr std::uint32_t TRACK         = 0x241724;
static constexpr std::uint32_t BORDER        = 0x422a43;
static constexpr std::uint32_t ACCENT        = 0x664167;
static constexpr std::uint32_t ACCENT_DETAIL = 0xb087b1;

static constexpr std::uint32_t TEXT          = 0xf4f4ef;
static constexpr std::uint32_t TEXT_SOFT     = 0xbcbcb5;
static constexpr std::uint32_t TEXT_MUTED    = 0xa0a09a;
static constexpr std::uint32_t TEXT_DIM      = 0x696964;

} // namespace BadgeTheme

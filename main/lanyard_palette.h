#pragma once

#include <cstdint>

// Canonical 2026 lanyard colours. Keep every role-colour UI tied to these
// values so the badge, LEDs, and Swag reference cannot drift apart.
namespace lanyard_palette {

constexpr std::uint32_t kHacker       = 0x4caf50;
constexpr std::uint32_t kVolunteer    = 0x65b9e8;
constexpr std::uint32_t kMentor       = 0xf08a28;
constexpr std::uint32_t kSponsor      = 0xd94343;
constexpr std::uint32_t kJudge        = 0xd94343;
constexpr std::uint32_t kOrganizer    = 0x24528a;
constexpr std::uint32_t kGeneral      = 0xf2d34f;
constexpr std::uint32_t kVisitor      = 0x4b5563;

} // namespace lanyard_palette

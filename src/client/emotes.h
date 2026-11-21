// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Emotes (C2S 0x05D MOTION / S2C 0x05A MOTIONMES): ids as LandSandBoat's Emote enum, and English log lines.
// The game's own emote sentences live in its text DATs (not located yet); these are this client's wording.
namespace xi::client
{

// "/wave" -> 8; nothing for unknown commands.
std::optional<uint8_t> emoteId(std::string_view command);

// The argument word of /bell and /jobemote -> C2S 0x05D Param (LSB 0x05d_motion.cpp): a bell note "C3" .. "C5" with "+"
// for sharps -> 6 .. 30; a job "war" .. "run" -> 0x1E + job id. 0 for none or unknown.
uint16_t emoteParam(uint8_t id, std::string_view argument);

// "Alice waves." / "Alice waves to Bob."
std::string emoteText(uint8_t id, const std::string& who, const std::string& target);

} // namespace xi::client

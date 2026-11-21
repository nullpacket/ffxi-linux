// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The names S2C 0x00E gives NPCs and monsters, and the server-side rename list (issue #138, docs/protocol-map.md
// "Entity names").
namespace xi::client
{

// What one 0x00E does to its entity's name, as FUNC_Packet_Incoming_0x000E does it (FFXiMain.dll 0x1009b596..0x1009b6e0).
// `d` is the whole sub-packet, header included. kind = look_t u16 at +0x30 & 7, mask = +0x0A, idx = ActIndex +0x08:
//   1. kind 0, 1, 5, 6 or 7, mask 0x08, idx >= 0x700: the name at +0x34 (strncpy, at most min(15, size - 0x38));
//   2. else kind 1, mask 0x40, idx >= 0x700: the name at +0x44 (at most min(15, size - 0x48)), LSB's renamed dynamic
//      entities with an equipped look (entity_update.cpp: size 0x56, mask 0x57);
//   3. else kind 1, mask 0x40, idx < 0x400, +0x44 > 0x20: the name at +0x44, likewise;
//   4. else idx < 0x400, mask 0x08, +0x34 == 1, +0x35 > 0x20: the name at +0x35 (LSB's renamed zone NPCs and mobs);
//   5. else idx < 0x700: the zone's name DAT -> "" here (the client's DAT fallback fills it in);
//   6. else (a dynamic entity whose update carries no name): unchanged -> nullopt.
// size is the header's size field x 4 (bytes), never more than d holds.
[[nodiscard]] std::optional<std::string> entityNameFromUpdate(std::span<const std::byte> d);

// The rename list a server can send (HorizonXI's, the protocol of its dynamic_entity_renamer addon). The client asks
// with an empty C2S 0x001 after zoning in; the server answers S2C 0x1FF whose text (after the 4-byte header, to the
// first NUL) is "zoneId|original:new|original:new|...". It names the zone's dynamic entities (ActIndex >= 0x700) by
// their original names. Cleared on every zone change.
class EntityRenames
{
public:
    // Parse a 0x1FF sub-packet (header included). False when it has no zone id; the list is then left as it was.
    bool parse(std::span<const std::byte> d);
    void clear()
    {
        zone_.reset();
        names_.clear();
    }
    [[nodiscard]] bool empty() const
    {
        return names_.empty();
    }
    // The new name for a dynamic entity in `zone` whose packet name is `packetName`, if the list renames it. The
    // addon keys an entity by its +0x34 name less the last character (`name:sub(1, -2)`; INFERRED why: HorizonXI's
    // server names are not observable here), and matches the list's original names against that.
    [[nodiscard]] std::optional<std::string> lookup(uint16_t zone, std::string_view packetName) const;
    [[nodiscard]] std::optional<uint16_t>    zone() const
    {
        return zone_;
    }

private:
    std::optional<uint16_t>            zone_;
    std::map<std::string, std::string> names_;
};

// C2S 0x001's payload (after the header) that asks for the list: four zero bytes, as the addon's {0x01, 0x04, 0, 0}
// header-only packet is padded to its size field (2 = 8 bytes).
inline constexpr std::size_t kRenameRequestPayload = 4;

} // namespace xi::client

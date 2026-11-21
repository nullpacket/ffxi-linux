// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "net/socket.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The search server (LSB src/search, TCP 54002): /search (/sea) player lists. One request per connection; the server
// answers with one or more list packets, the last flagged final.
//
// Framing, both ways: u16 size @0, "IXFF" @4 (responses), type @0x0B (requests); a body; MD5 of bytes 8 .. size-0x14
// at size-0x14; a 4-byte key at size-4. Bytes 8 .. are Blowfish (FFXI variant) in 8-byte blocks:
//   request key  = MD5(static 16 bytes + the request's key)
//   response key = MD5(static 16 bytes + the request's key + the request's plaintext at size-0x18, 4 bytes)
// Request body: a bit-packed (MSB-first) criteria list at 0x11, its byte length at 0x10. Each entry: 5-bit type, 1-bit
// sort, 1-bit present, then the value (name: 5-bit length + 7-bit chars; area: 10 bits; job: 5; level: 8 + 8).
// Response body: u16 data size @8, final flag 0x80 @0x0A, u16 total found @0x0E, entries from byte 24: u8 entry size,
// then bit fields (type 5 bits + value; name: 4-bit length + 7-bit chars).
namespace xi::client
{

struct SearchQuery
{
    std::optional<uint16_t> zone; // only this area (plain /sea); none = everywhere (/sea all)
    std::string             name; // name filter
    uint8_t                 job    = 0;
    uint8_t                 minLvl = 0, maxLvl = 0;
    std::optional<uint8_t>  nation; // 0 San d'Oria, 1 Bastok, 2 Windurst (type 0x02, 2 bits)
    std::optional<uint8_t>  race;   // 0 Hume, 1 Elvaan, 2 Tarutaru, 3 Mithra, 4 Galka (type 0x05, 4 bits)
    uint8_t                 minRank = 0, maxRank = 0; // type 0x10, 8 + 8 bits
    uint32_t                comment = 0; // comment genre << 4 (type 0x11, 32 bits; LSB matches seacom_type & 0xF0)
    uint32_t                linkshell  = 0;     // members of this linkshell (type 0x0B, or 0x13 for the second; 32 bits)
    bool                    linkshell2 = false;
    // Flags 2 (type 0x16, 32 bits; CODE 0x10104130 sends it last): a Unity leader << 22 (0x100e3130), 0x10000000 party
    // requests, 0x20000000 the Mute List (/mutelist), 0xF0000 Ballista. LSB tests bits 22.. as a Unity id first.
    uint32_t                flags2 = 0;
};

struct SearchHit
{
    std::string name;
    uint16_t    zone   = 0;
    uint8_t     nation = 0, mainJob = 0, subJob = 0, mainLevel = 0, subLevel = 0, race = 0, rank = 0;
    uint16_t    flags1 = 0; // 0x4000: anonymous (no job, level, nation, race or rank sent)
    uint32_t    id     = 0;
    uint32_t    commentType = 0; // search comment category (C2S 0x0E0 msgType), 0 = none
    uint32_t    flags2      = 0; // type 0x16: flags 1 plus 0x10000000 request, 0x20000000 muted (LSB)
    [[nodiscard]] bool anonymous() const
    {
        return (flags1 & 0x4000) != 0;
    }
};

struct SearchResult
{
    uint32_t               total = 0; // matches found (the list can be shorter)
    std::vector<SearchHit> hits;
};

// The encrypted request, and the plaintext word the response key needs (exposed for tests).
struct SearchRequest
{
    std::vector<std::byte> bytes;
    std::array<std::byte, 24> responseKey{};
};
SearchRequest buildSearchRequest(const SearchQuery& q, uint32_t key);
// Any request: `body` holds the plaintext from byte 0 (type @0x0B ...); this adds size, "IXFF", hash and key, and
// encrypts.
SearchRequest sealRequest(std::vector<std::byte> body, uint32_t key);
// Decrypts an answer packet in place.
void decryptResponse(std::span<std::byte> packet, const std::array<std::byte, 24>& responseKey);
// Decrypts one response packet in place and appends its entries; returns true when it was the last one.
bool readSearchList(std::span<std::byte> packet, const std::array<std::byte, 24>& responseKey, SearchResult& out);

// Connects, asks and collects every list packet. Throws on network errors.
SearchResult searchPlayers(const net::Endpoint& server, const SearchQuery& q);

// Auction house lists, from the same server. A category's items with the stock on auction (TCP_AH_REQUEST 0x15,
// category @0x16; answers type 0x95, 20 items of {u16 id, u32 single, u32 stack} from 0x18, final flag 0x80 @0x0A).
struct AuctionItem
{
    uint16_t itemId = 0;
    uint32_t single = 0, stack = 0;
};
// A list query's sort / filter term (the game's query terms, CODE 0x101040a0 / 0x10104010; at most 20, from 0x18): type
// 2 level, 3 race (filter), 4 job (filter), 5 damage, 6 delay, 7 defense, 8 elemental resistance, 9 name; bits: bit 0
// has a value, bit 1 descending, bits 2..16 the value.
struct AuctionTerm
{
    uint32_t type = 0, bits = 0;
};
std::vector<AuctionItem> auctionList(const net::Endpoint& server, uint8_t category, const std::vector<AuctionTerm>& terms = {});
bool readAuctionList(std::span<std::byte> packet, const std::array<std::byte, 24>& responseKey, std::vector<AuctionItem>& out);
// An item's last ten sales (TCP_AH_HISTORY_SINGLE 5 / STACK 6, item @0x12; answer 0x85: 40-byte records from 0x20
// {u32 price, u32 date, seller[16], buyer[16]}).
struct AuctionSale
{
    uint32_t    price = 0, date = 0;
    std::string seller, buyer;
};
std::vector<AuctionSale> auctionHistory(const net::Endpoint& server, uint16_t itemId, bool stack);
std::vector<AuctionSale> readAuctionHistory(std::span<std::byte> packet, const std::array<std::byte, 24>& responseKey);

} // namespace xi::client

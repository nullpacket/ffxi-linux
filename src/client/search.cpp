// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/search.h"

#include "xi/blowfish.h"
#include "xi/md5.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace xi::client
{

namespace
{

// LSB search_handler.h `key`: the first 16 bytes of every search key.
constexpr std::array<uint8_t, 16> kStaticKey = { 0x30, 0x73, 0x3D, 0x6D, 0x3C, 0x31, 0x49, 0x5A, 0x32, 0x7A, 0x42, 0x43, 0x63, 0x38, 0x7B, 0x7E };

// The bit stream (LSB packBitsLE / unpackBitsLE, despite the names): each value most significant bit first, bytes
// filled from their top bit down (DATA: a list entry starts 03 42 = type 0, name length 6, 'B').
void putBits(std::vector<uint8_t>& out, uint32_t& at, uint64_t value, int n)
{
    for (int k = n - 1; k >= 0; --k, ++at)
    {
        if (at / 8 >= out.size())
        {
            out.resize(at / 8 + 1);
        }
        if ((value >> k) & 1)
        {
            out[at / 8] = static_cast<uint8_t>(out[at / 8] | (0x80u >> (at % 8)));
        }
    }
}

uint64_t getBits(std::span<const std::byte> in, uint32_t& at, int n)
{
    uint64_t v = 0;
    for (int k = 0; k < n; ++k, ++at)
    {
        const bool bit = at / 8 < in.size() && ((static_cast<uint8_t>(in[at / 8]) << (at % 8)) & 0x80);
        v              = (v << 1) | (bit ? 1 : 0);
    }
    return v;
}

Blowfish cipher(std::span<const std::byte> key)
{
    Blowfish bf(Blowfish::SessionKey{});
    const auto hash = md5(key);
    bf.schedule(hash);
    return bf;
}

// Bytes 8 .. as LSB does: (size - 12) / 4 words, rounded down to whole 8-byte blocks. LSB counts a response's words
// in a uint8 (search_handler.cpp encrypt), so a long list packet is only encrypted that far.
std::span<std::byte> cipherSpan(std::span<std::byte> p, bool response = false)
{
    std::size_t words = (p.size() - 12) / 4;
    if (response)
    {
        words &= 0xFF;
    }
    words -= words % 2;
    return p.subspan(8, words * 4);
}

} // namespace

SearchRequest buildSearchRequest(const SearchQuery& q, uint32_t key)
{
    std::vector<uint8_t> bits;
    uint32_t             at = 0;
    auto entry = [&](uint8_t type) {
        putBits(bits, at, type, 5);
        putBits(bits, at, 0, 1); // ascending
        putBits(bits, at, 1, 1); // present
    };
    // Friend, Linkshell, Linkshell 2, Comment and Flags2 entries have no sort / present bits (LSB search_handler).
    auto bare = [&](uint8_t type) { putBits(bits, at, type, 5); };
    if (!q.name.empty())
    {
        const auto n = std::min<std::size_t>(q.name.size(), 15);
        entry(0x00);
        putBits(bits, at, n, 5);
        for (std::size_t i = 0; i < n; ++i)
        {
            putBits(bits, at, static_cast<uint8_t>(q.name[i]) & 0x7F, 7);
        }
    }
    if (q.zone)
    {
        entry(0x01);
        putBits(bits, at, *q.zone, 10);
    }
    if (q.job)
    {
        entry(0x03);
        putBits(bits, at, q.job, 5);
    }
    if (q.nation)
    {
        entry(0x02);
        putBits(bits, at, *q.nation, 2);
    }
    if (q.race)
    {
        entry(0x05);
        putBits(bits, at, *q.race, 4);
    }
    if (q.minRank || q.maxRank)
    {
        entry(0x10);
        putBits(bits, at, q.minRank, 8);
        putBits(bits, at, q.maxRank ? q.maxRank : q.minRank, 8);
    }
    if (q.comment)
    {
        bare(0x11);
        putBits(bits, at, q.comment, 32);
    }
    if (q.linkshell)
    {
        bare(q.linkshell2 ? 0x13 : 0x0B);
        putBits(bits, at, q.linkshell, 32);
    }
    if (q.minLvl || q.maxLvl)
    {
        entry(0x04);
        putBits(bits, at, q.minLvl, 8);
        putBits(bits, at, q.maxLvl ? q.maxLvl : 99, 8);
    }
    if (q.flags2)
    {
        bare(0x16);
        putBits(bits, at, q.flags2, 32);
    }
    if (bits.empty())
    {
        bits.push_back(0);
    }
    std::vector<std::byte> body(0x11 + bits.size());
    body[0x0B] = static_cast<std::byte>(q.zone ? 0x03 : 0x00); // TCP_SEARCH / TCP_SEARCH_ALL
    body[0x10] = static_cast<std::byte>(bits.size());
    std::memcpy(body.data() + 0x11, bits.data(), bits.size());
    return sealRequest(std::move(body), key);
}

SearchRequest sealRequest(std::vector<std::byte> body, uint32_t key)
{
    // The body, a 4-byte word, the hash and the key; padded so the cipher covers everything up to the key.
    std::size_t size = std::max<std::size_t>(body.size(), 0x18) + 4 + 0x14;
    while (size % 8 != 4)
    {
        ++size;
    }
    SearchRequest r;
    auto&         p = r.bytes;
    p              = std::move(body);
    p.resize(size);
    const auto s16 = static_cast<uint16_t>(size);
    std::memcpy(p.data(), &s16, 2);
    std::memcpy(p.data() + 4, "IXFF", 4);
    const auto hash = md5(std::span<const std::byte>(p).subspan(8, size - 0x14 - 8));
    std::memcpy(p.data() + size - 0x14, hash.data(), 16);
    std::memcpy(p.data() + size - 4, &key, 4);

    std::array<std::byte, 20> reqKey{};
    std::memcpy(reqKey.data(), kStaticKey.data(), 16);
    std::memcpy(reqKey.data() + 16, &key, 4);
    std::memcpy(r.responseKey.data(), reqKey.data(), 20);
    std::memcpy(r.responseKey.data() + 20, p.data() + size - 0x18, 4);
    cipher(reqKey).encryptBlocks(cipherSpan(p));
    return r;
}

void decryptResponse(std::span<std::byte> p, const std::array<std::byte, 24>& responseKey)
{
    if (p.size() < 24 + 0x14)
    {
        throw std::runtime_error("search: short packet");
    }
    cipher(responseKey).decryptBlocks(cipherSpan(p, true));
}

bool readSearchList(std::span<std::byte> p, const std::array<std::byte, 24>& responseKey, SearchResult& out)
{
    decryptResponse(p, responseKey);
    const bool final = static_cast<uint8_t>(p[0x0A]) & 0x80;
    uint16_t   end   = 0;
    std::memcpy(&end, p.data() + 8, 2);
    uint16_t total = 0;
    std::memcpy(&total, p.data() + 0x0E, 2);
    out.total = total;
    end       = static_cast<uint16_t>(std::min<std::size_t>(end, p.size() - 0x14));

    std::size_t at = 24;
    while (at < end)
    {
        const auto              len  = static_cast<uint8_t>(p[at]);
        const auto              body = std::span<const std::byte>(p).subspan(at + 1, std::min<std::size_t>(len, end - at - 1));
        uint32_t                bit  = 0;
        const uint32_t          bits = static_cast<uint32_t>(body.size() * 8);
        SearchHit               h;
        while (bit + 5 <= bits)
        {
            const auto type = static_cast<uint8_t>(getBits(body, bit, 5));
            switch (type)
            {
                case 0x00: // name
                {
                    const auto n = getBits(body, bit, 4);
                    for (uint64_t k = 0; k < n; ++k)
                    {
                        h.name += static_cast<char>(getBits(body, bit, 7));
                    }
                    break;
                }
                case 0x01: h.zone = static_cast<uint16_t>(getBits(body, bit, 10)); break;
                case 0x02: h.nation = static_cast<uint8_t>(getBits(body, bit, 2)); break;
                case 0x03:
                    h.mainJob = static_cast<uint8_t>(getBits(body, bit, 5));
                    h.subJob  = static_cast<uint8_t>(getBits(body, bit, 5));
                    break;
                case 0x04:
                    h.mainLevel = static_cast<uint8_t>(getBits(body, bit, 8));
                    h.subLevel  = static_cast<uint8_t>(getBits(body, bit, 8));
                    break;
                case 0x05: h.race = static_cast<uint8_t>(getBits(body, bit, 4)); break;
                case 0x06: h.flags1 = static_cast<uint16_t>(getBits(body, bit, 16)); break;
                case 0x08: h.id = static_cast<uint32_t>(getBits(body, bit, 20)); break;
                case 0x10: h.rank = static_cast<uint8_t>(getBits(body, bit, 8)); break;
                case 0x0D: getBits(body, bit, 8); break;  // linkshell rank
                case 0x11: h.commentType = static_cast<uint32_t>(getBits(body, bit, 32)); break;
                case 0x0E: getBits(body, bit, 32); break; // unknown
                case 0x16: h.flags2 = static_cast<uint32_t>(getBits(body, bit, 32)); break;
                case 0x17: getBits(body, bit, 16); break; // languages
                default: bit = bits; break;               // unknown: skip the rest of the entry
            }
        }
        if (!h.name.empty())
        {
            out.hits.push_back(std::move(h));
        }
        at += 1 + len;
    }
    return final;
}

namespace
{
uint32_t freshKey()
{
    return static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count() * 2654435761u);
}

// Sends a sealed request and hands each answer packet (decrypted) to `each` until it returns true (the last one).
template <typename F>
void exchange(const net::Endpoint& server, const SearchRequest& req, F&& each)
{
    auto stream = net::TcpStream::connect(server);
    stream.sendAll(req.bytes);
    for (int packets = 0; packets < 64; ++packets)
    {
        std::array<std::byte, 2> head{};
        stream.recvExact(head);
        uint16_t size = 0;
        std::memcpy(&size, head.data(), 2);
        if (size < 24 + 0x14)
        {
            throw std::runtime_error("search: bad packet size");
        }
        std::vector<std::byte> p(size);
        std::memcpy(p.data(), head.data(), 2);
        stream.recvExact(std::span(p).subspan(2));
        if (each(std::span<std::byte>(p)))
        {
            break;
        }
    }
}
} // namespace

SearchResult searchPlayers(const net::Endpoint& server, const SearchQuery& q)
{
    const auto   req = buildSearchRequest(q, freshKey());
    SearchResult out;
    exchange(server, req, [&](std::span<std::byte> p) { return readSearchList(p, req.responseKey, out); });
    return out;
}

bool readAuctionList(std::span<std::byte> p, const std::array<std::byte, 24>& responseKey, std::vector<AuctionItem>& out)
{
    decryptResponse(p, responseKey);
    uint16_t end = 0;
    std::memcpy(&end, p.data() + 8, 2);
    end = static_cast<uint16_t>(std::min<std::size_t>(end, p.size() - 0x14));
    for (std::size_t at = 0x18; at + 10 <= end; at += 10)
    {
        AuctionItem it;
        std::memcpy(&it.itemId, p.data() + at, 2);
        std::memcpy(&it.single, p.data() + at + 2, 4);
        std::memcpy(&it.stack, p.data() + at + 6, 4);
        out.push_back(it);
    }
    return static_cast<uint8_t>(p[0x0A]) & 0x80;
}

std::vector<AuctionItem> auctionList(const net::Endpoint& server, uint8_t category, const std::vector<AuctionTerm>& terms)
{
    // TCP_AH_REQUEST: type 0x15 @0x0B, term count @0x12, category @0x16, the terms {u32 type, u32 bits} from 0x18 (the
    // game's list query struct at +0x10: count +2, category +6, terms +8; CODE 0x10103ec0 / 0x101040a0).
    const std::size_t      n = std::min<std::size_t>(terms.size(), 20);
    std::vector<std::byte> body(0x18 + 8 * n);
    body[0x0B]     = std::byte{ 0x15 };
    body[0x12]     = static_cast<std::byte>(n);
    body[0x16]     = static_cast<std::byte>(category);
    for (std::size_t i = 0; i < n; ++i)
    {
        std::memcpy(body.data() + 0x18 + 8 * i, &terms[i].type, 4);
        std::memcpy(body.data() + 0x1C + 8 * i, &terms[i].bits, 4);
    }
    const auto req = sealRequest(std::move(body), freshKey());
    std::vector<AuctionItem> out;
    exchange(server, req, [&](std::span<std::byte> p) { return readAuctionList(p, req.responseKey, out); });
    return out;
}

std::vector<AuctionSale> readAuctionHistory(std::span<std::byte> p, const std::array<std::byte, 24>& responseKey)
{
    decryptResponse(p, responseKey);
    uint16_t end = 0;
    std::memcpy(&end, p.data() + 8, 2);
    end = static_cast<uint16_t>(std::min<std::size_t>(end, p.size() - 0x14));
    std::vector<AuctionSale> out;
    auto name = [&](std::size_t at) {
        const auto* c = reinterpret_cast<const char*>(p.data() + at);
        return std::string(c, strnlen(c, 15));
    };
    for (std::size_t at = 0x20; at + 40 <= end; at += 40)
    {
        AuctionSale s;
        std::memcpy(&s.price, p.data() + at, 4);
        std::memcpy(&s.date, p.data() + at + 4, 4);
        s.seller = name(at + 8);
        s.buyer  = name(at + 0x18);
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<AuctionSale> auctionHistory(const net::Endpoint& server, uint16_t itemId, bool stack)
{
    // TCP_AH_HISTORY_SINGLE (5) / _STACK (6): item id @0x12, stack flag @0x15.
    std::vector<std::byte> body(0x18);
    body[0x0B] = static_cast<std::byte>(stack ? 0x06 : 0x05);
    std::memcpy(body.data() + 0x12, &itemId, 2);
    body[0x15]     = static_cast<std::byte>(stack ? 1 : 0);
    const auto req = sealRequest(std::move(body), freshKey());
    std::vector<AuctionSale> out;
    exchange(server, req, [&](std::span<std::byte> p) {
        out = readAuctionHistory(p, req.responseKey);
        return true;
    });
    return out;
}

} // namespace xi::client

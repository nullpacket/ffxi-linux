// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/huffman.h"

#include <fmt/format.h>

#include <fstream>
#include <stdexcept>

namespace xi
{

Huffman Huffman::fromCompressDat(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    std::array<uint32_t, 512> table{};
    if (!f.read(reinterpret_cast<char*>(table.data()), sizeof(table)))
    {
        throw std::runtime_error(fmt::format("cannot read 2048 bytes from {}", path.string()));
    }
    return fromTable(table);
}

Huffman Huffman::fromTable(std::span<const uint32_t, 512> table)
{
    Huffman h;
    for (unsigned b = 0; b < 256; ++b)
    {
        const int      idx = static_cast<int8_t>(b) + 0x80;
        const uint32_t len = table[static_cast<std::size_t>(idx + 0x100)];
        if (len == 0 || len > kMaxBits)
        {
            throw std::runtime_error(fmt::format("compress table: symbol {:#04x} has code length {}", b, len));
        }
        h.len_[b]  = static_cast<uint8_t>(len);
        h.code_[b] = table[static_cast<std::size_t>(idx)] & ((1u << len) - 1);

        // Every 12-bit window whose low `len` bits equal the code decodes to this symbol.
        for (uint32_t hi = 0; hi < (1u << (kMaxBits - len)); ++hi)
        {
            auto& e = h.lut_[h.code_[b] | (hi << len)];
            if (e.length != 0)
            {
                throw std::runtime_error("compress table is not prefix-free");
            }
            e = { static_cast<uint8_t>(b), static_cast<uint8_t>(len) };
        }
    }
    for (const auto& e : h.lut_)
    {
        if (e.length == 0)
        {
            throw std::runtime_error("compress table is not a complete code");
        }
    }
    return h;
}

std::vector<std::byte> Huffman::decode(std::span<const std::byte> body, uint32_t bitCount) const
{
    if (body.empty() || body[0] != std::byte{ 0x01 })
    {
        throw std::runtime_error("huffman: missing 0x01 flag");
    }
    if (bitCount < 8 || (bitCount + 7) / 8 > body.size())
    {
        throw std::runtime_error(fmt::format("huffman: bitCount {} out of range for {} bytes", bitCount, body.size()));
    }
    const auto     stream = body.subspan(1);
    const uint32_t total  = bitCount - 8;

    std::vector<std::byte> out;
    out.reserve(total / 4);
    uint32_t pos = 0;
    while (pos < total)
    {
        // Gather the next 12 bits LSB-first (zero beyond the end).
        uint32_t window = 0;
        for (unsigned i = 0; i < kMaxBits; ++i)
        {
            const uint32_t bit = pos + i;
            if (bit >= total)
            {
                break;
            }
            window |= ((std::to_integer<uint32_t>(stream[bit >> 3]) >> (bit & 7)) & 1u) << i;
        }
        const auto& e = lut_[window];
        if (pos + e.length > total)
        {
            throw std::runtime_error("huffman: truncated code at end of stream");
        }
        out.push_back(std::byte{ e.symbol });
        pos += e.length;
    }
    return out;
}

std::vector<std::byte> Huffman::encode(std::span<const std::byte> data, uint32_t& bitCount) const
{
    std::vector<std::byte> out{ std::byte{ 0x01 } };
    uint32_t               pos = 0;
    for (const auto b : data)
    {
        const auto sym = std::to_integer<uint8_t>(b);
        for (unsigned i = 0; i < len_[sym]; ++i, ++pos)
        {
            if ((pos >> 3) + 1 >= out.size())
            {
                out.push_back(std::byte{ 0 });
            }
            if ((code_[sym] >> i) & 1u)
            {
                out[1 + (pos >> 3)] |= std::byte{ static_cast<uint8_t>(1u << (pos & 7)) };
            }
        }
    }
    bitCount = pos + 8;
    return out;
}

} // namespace xi

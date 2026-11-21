// SPDX-License-Identifier: GPL-3.0-or-later
#include "xi/bytes.h"

#include <fmt/format.h>

namespace xi
{

std::string hexdump(std::span<const std::byte> buf)
{
    std::string out;
    for (std::size_t row = 0; row < buf.size(); row += 16)
    {
        out += fmt::format("{:04x}  ", row);
        for (std::size_t i = row; i < row + 16; ++i)
        {
            out += i < buf.size() ? fmt::format("{:02x} ", std::to_integer<unsigned>(buf[i])) : "   ";
        }
        out += ' ';
        for (std::size_t i = row; i < row + 16 && i < buf.size(); ++i)
        {
            const auto c = std::to_integer<unsigned char>(buf[i]);
            out += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
        }
        out += '\n';
    }
    return out;
}

} // namespace xi

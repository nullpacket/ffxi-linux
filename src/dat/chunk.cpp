// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/chunk.h"

#include "xi/bytes.h"

namespace xi::dat
{

bool parseChunks(std::span<const std::byte> file, std::vector<Chunk>& out)
{
    out.clear();
    std::size_t off = 0;
    while (off + 16 <= file.size())
    {
        const auto info = load<uint32_t>(file, off + 4);
        const auto size = static_cast<std::size_t>((info >> 7) & 0x7FFFF) * 16;
        if (size < 16 || off + size > file.size())
        {
            return false;
        }
        out.push_back({ loadString(file, off, 4), static_cast<uint8_t>(info & 0x7F), static_cast<uint8_t>(info >> 26), off, file.subspan(off + 16, size - 16) });
        off += size;
    }
    return off == file.size() && !out.empty();
}

} // namespace xi::dat

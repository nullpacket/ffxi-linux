// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xi::dat
{

// One chunk of a chunked DAT file. Header (16 bytes): char name[4]; u32 info; 8 bytes reserved.
// type = info & 0x7F; total size including header = ((info >> 7) & 0x7FFFF) * 16; the top bits carry flags
// (bits 28 and 31 observed). The 19-bit width is the narrowest that walks every sampled file (584/600
// random files reach EOF; the rest are other formats: PNG, SQLE, d_msg, raw) — pending confirmation
// from the client's chunk reader. Role of each type: docs/dat-format.md.
struct Chunk
{
    std::string                name; // up to 4 chars, NUL-trimmed
    uint8_t                    type = 0;
    uint8_t                    flags = 0;  // info >> 26
    std::size_t                offset = 0; // of the header, within the file
    std::span<const std::byte> payload;    // bytes after the 16-byte header
};

// Walks the whole file. Returns false (with what it parsed so far in `out`) if the file is not a
// well-formed chunk sequence ending exactly at EOF.
bool parseChunks(std::span<const std::byte> file, std::vector<Chunk>& out);
// The chunks point into `file`, so a temporary (`parseChunks(index.read(id), ...)`) would leave them dangling.
bool parseChunks(std::vector<std::byte>&& file, std::vector<Chunk>& out) = delete;

} // namespace xi::dat

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace xi::dat
{

// Resolves FFXI numeric file ids to DAT paths in an install directory.
//
// ROM 1 uses VTABLE.DAT / FTABLE.DAT in the install root; ROM n (n >= 2) uses ROMn/VTABLEn.DAT and
// ROMn/FTABLEn.DAT. VTABLE[id] is the ROM number holding the file (0 = absent); FTABLE[id] is a u16
// packing (dir << 7 | file), giving <ROM dir>/<dir>/<file>.DAT. Verified against a full install: every
// entry resolves and no id appears in more than one ROM.
class DatIndex
{
public:
    explicit DatIndex(std::filesystem::path installDir);

    [[nodiscard]] std::optional<std::filesystem::path> path(uint32_t fileId) const;
    [[nodiscard]] std::vector<std::byte>                read(uint32_t fileId) const;
    // `size` bytes from `offset` (fewer at the end of the file).
    [[nodiscard]] std::vector<std::byte> read(uint32_t fileId, uint64_t offset, std::size_t size) const;

    [[nodiscard]] uint32_t size() const
    {
        return static_cast<uint32_t>(rom_.size());
    }
    [[nodiscard]] uint32_t presentCount() const;

    [[nodiscard]] const std::filesystem::path& installDir() const
    {
        return root_;
    }

private:
    std::filesystem::path root_;
    std::vector<uint8_t>  rom_;  // per id: ROM number, 0 = absent
    std::vector<uint16_t> loc_;  // per id: dir << 7 | file
};

} // namespace xi::dat

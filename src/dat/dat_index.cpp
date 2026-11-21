// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/dat_index.h"

#include <fmt/format.h>

#include <fstream>
#include <stdexcept>

namespace xi::dat
{

namespace
{

std::vector<std::byte> readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)
    {
        throw std::runtime_error(fmt::format("cannot open {}", p.string()));
    }
    std::vector<std::byte> buf(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    return buf;
}

std::filesystem::path romDir(const std::filesystem::path& root, unsigned rom)
{
    return root / (rom == 1 ? std::string("ROM") : fmt::format("ROM{}", rom));
}

} // namespace

DatIndex::DatIndex(std::filesystem::path installDir)
: root_(std::move(installDir))
{
    for (unsigned rom = 1;; ++rom)
    {
        const auto dir   = rom == 1 ? root_ : romDir(root_, rom);
        const auto vpath = dir / (rom == 1 ? std::string("VTABLE.DAT") : fmt::format("VTABLE{}.DAT", rom));
        const auto fpath = dir / (rom == 1 ? std::string("FTABLE.DAT") : fmt::format("FTABLE{}.DAT", rom));
        if (!std::filesystem::exists(vpath))
        {
            if (rom == 1)
            {
                throw std::runtime_error(fmt::format("{} is not an FFXI install (no VTABLE.DAT)", root_.string()));
            }
            break;
        }
        const auto v = readFile(vpath);
        const auto f = readFile(fpath);
        if (f.size() != v.size() * 2)
        {
            throw std::runtime_error(fmt::format("{}: FTABLE/VTABLE size mismatch", dir.string()));
        }
        if (rom_.size() < v.size())
        {
            rom_.resize(v.size());
            loc_.resize(v.size());
        }
        for (std::size_t id = 0; id < v.size(); ++id)
        {
            if (std::to_integer<uint8_t>(v[id]) != 0 && rom_[id] == 0)
            {
                rom_[id] = static_cast<uint8_t>(rom);
                loc_[id] = static_cast<uint16_t>(std::to_integer<uint16_t>(f[2 * id]) | (std::to_integer<uint16_t>(f[2 * id + 1]) << 8));
            }
        }
    }
}

std::optional<std::filesystem::path> DatIndex::path(uint32_t fileId) const
{
    if (fileId >= rom_.size() || rom_[fileId] == 0)
    {
        return std::nullopt;
    }
    const auto e = loc_[fileId];
    return romDir(root_, rom_[fileId]) / std::to_string(e >> 7) / fmt::format("{}.DAT", e & 0x7F);
}

std::vector<std::byte> DatIndex::read(uint32_t fileId) const
{
    const auto p = path(fileId);
    if (!p)
    {
        throw std::runtime_error(fmt::format("file id {} not present in install", fileId));
    }
    return readFile(*p);
}

std::vector<std::byte> DatIndex::read(uint32_t fileId, uint64_t offset, std::size_t size) const
{
    const auto p = path(fileId);
    if (!p)
    {
        throw std::runtime_error(fmt::format("file id {} not present in install", fileId));
    }
    std::ifstream f(*p, std::ios::binary);
    std::vector<std::byte> out(size);
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    out.resize(static_cast<std::size_t>(std::max<std::streamsize>(f.gcount(), 0)));
    return out;
}

uint32_t DatIndex::presentCount() const
{
    uint32_t n = 0;
    for (const auto r : rom_)
    {
        n += r != 0;
    }
    return n;
}

} // namespace xi::dat

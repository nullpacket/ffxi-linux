// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/macro_file.h"

#include "xi/md5.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace xi::dat::macro
{

namespace
{

// The header's checksum covers everything after it.
bool checksumOk(std::span<const std::byte> file)
{
    if (file.size() < kHeaderBytes)
    {
        return false;
    }
    const auto sum = md5(file.subspan(kHeaderBytes));
    return std::memcmp(sum.data(), file.data() + 8, sum.size()) == 0;
}

std::vector<std::byte> withHeader(std::vector<std::byte> payload)
{
    std::vector<std::byte> out(kHeaderBytes);
    out[0]         = std::byte{ 1 };
    const auto sum = md5(payload);
    std::memcpy(out.data() + 8, sum.data(), sum.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// A NUL-terminated field of `size` bytes.
std::string field(std::span<const std::byte> at, std::size_t size)
{
    const auto* p = reinterpret_cast<const char*>(at.data());
    return std::string(p, strnlen(p, size));
}

void putField(std::byte* at, std::string_view s, std::size_t size)
{
    std::memcpy(at, s.data(), std::min(s.size(), size - 1));
}

} // namespace

bool Macro::empty() const
{
    return name.empty() && std::all_of(lines.begin(), lines.end(), [](const std::string& l) { return l.empty(); });
}

std::string setFileName(int book, int set)
{
    const int index = book * kSets + set;
    return index == 0 ? std::string("mcr.dat") : "mcr" + std::to_string(index) + ".dat";
}

std::optional<Set> parseSet(std::span<const std::byte> file)
{
    if (file.size() != kHeaderBytes + 2 * kKeys * kMacroBytes || !checksumOk(file))
    {
        return std::nullopt;
    }
    Set s;
    for (std::size_t m = 0; m < s.macros.size(); ++m)
    {
        const auto rec = file.subspan(kHeaderBytes + m * kMacroBytes, kMacroBytes);
        auto&      mac = s.macros[m];
        std::memcpy(&mac.flags, rec.data(), 4);
        for (std::size_t l = 0; l < kLines; ++l)
        {
            mac.lines[l] = field(rec.subspan(4 + l * kLineBytes), kLineBytes);
        }
        mac.name = field(rec.subspan(4 + kLines * kLineBytes), kNameBytes);
    }
    return s;
}

std::vector<std::byte> writeSet(const Set& set)
{
    std::vector<std::byte> payload(2 * kKeys * kMacroBytes);
    for (std::size_t m = 0; m < set.macros.size(); ++m)
    {
        std::byte*  rec = payload.data() + m * kMacroBytes;
        const auto& mac = set.macros[m];
        std::memcpy(rec, &mac.flags, 4);
        for (std::size_t l = 0; l < kLines; ++l)
        {
            putField(rec + 4 + l * kLineBytes, mac.lines[l], kLineBytes);
        }
        putField(rec + 4 + kLines * kLineBytes, mac.name, kNameBytes);
    }
    return withHeader(std::move(payload));
}

std::optional<std::array<std::string, 20>> parseTitles(std::span<const std::byte> file)
{
    if (file.size() != kHeaderBytes + 20 * 16 || !checksumOk(file))
    {
        return std::nullopt;
    }
    std::array<std::string, 20> names;
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        names[i] = field(file.subspan(kHeaderBytes + i * 16), 16);
    }
    return names;
}

std::vector<std::byte> writeTitles(const std::array<std::string, 20>& names)
{
    std::vector<std::byte> payload(20 * 16);
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        putField(payload.data() + i * 16, names[i], 16);
    }
    return withHeader(std::move(payload));
}

std::optional<uint32_t> parseSys(std::span<const std::byte> file)
{
    if (file.size() != kHeaderBytes + 4 || !checksumOk(file))
    {
        return std::nullopt;
    }
    uint32_t v = 0;
    std::memcpy(&v, file.data() + kHeaderBytes, 4);
    return v;
}

std::vector<std::byte> writeSys(uint32_t value)
{
    std::vector<std::byte> payload(4);
    std::memcpy(payload.data(), &value, 4);
    return withHeader(std::move(payload));
}

LineStep stepFor(std::string_view line)
{
    LineStep s;
    if (line.empty())
    {
        return s; // End
    }
    std::string low(line);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (low.starts_with("//"))
    {
        s.kind = LineStep::Kind::Skip;
        return s;
    }
    s.kind = LineStep::Kind::Run;
    if (low == "/wait" || low.starts_with("/wait "))
    {
        const int n = low.size() > 6 ? std::atoi(low.c_str() + 6) : 1;
        s.wait      = n >= 1 && n <= 60 ? n : 0;
        return s; // nothing to run
    }
    if (const auto w = low.find("<wait"); w != std::string::npos && low.find('>', w) != std::string::npos)
    {
        const auto close = low.find('>', w);
        const auto inner = low.substr(w + 5, close - w - 5);
        const int  n     = inner.find_first_not_of(' ') == std::string::npos ? 1 : std::atoi(inner.c_str());
        s.wait           = std::clamp(n, 0, 60);
        s.text           = std::string(line.substr(0, w));
        while (!s.text.empty() && s.text.back() == ' ')
        {
            s.text.pop_back();
        }
        return s;
    }
    s.text = std::string(line);
    return s;
}

} // namespace xi::dat::macro

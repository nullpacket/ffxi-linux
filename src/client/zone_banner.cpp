// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/zone_banner.h"

#include "dat/messages.h"

#include <string_view>

namespace xi::client
{

namespace
{

// The game's printf with one argument: the first %s / %d takes it.
std::string format1(std::string_view fmt, std::string_view arg)
{
    std::string out(fmt);
    for (const char* spec : { "%s", "%d" })
    {
        if (const auto p = out.find(spec); p != std::string::npos)
        {
            out.replace(p, 2, arg);
            break;
        }
    }
    return out;
}

void areaLines(const ZoneInInfo& z, const dat::XiStringTable* formats, const dat::StringTable* areas, std::vector<std::string>& out)
{
    const std::string_view name = areas ? areas->get(z.zone) : std::string_view();
    if (name.empty())
    {
        out.push_back(format1("=== Area: %d ===", std::to_string(z.zone))); // DATA 0x10362354
        return;
    }
    if (!formats)
    {
        return;
    }
    if (const auto pre = formats->get(0x5E); !pre.empty())
    {
        out.push_back(format1(pre, name));
    }
    if (const auto line = formats->get(0x62); !line.empty())
    {
        out.push_back(format1(line, name));
    }
}

} // namespace

std::vector<std::string> zoneInLines(const ZoneInInfo& z, const dat::XiStringTable* formats, const dat::StringTable* areas,
                                     const dat::StringTable* clientText)
{
    std::vector<std::string> out;
    switch (z.loginState)
    {
        case 1:
            if (z.mapNumber == 0x118 || z.mapNumber == 0x11D)
            {
                areaLines(z, formats, areas, out);
            }
            else if ((z.mhFloor != 0 || (z.mhMap > 0x120 && z.mhMap < 0x124)) && z.mhFloor < 2 && clientText)
            {
                // Table 0x10331820: floor 0 -> 294, 1 -> 295 (the other two slots are not string ids).
                if (const auto line = clientText->get(294u + z.mhFloor); !line.empty())
                {
                    out.emplace_back(line);
                }
            }
            break;
        case 3:
        case 4:
        case 5: break;
        default: areaLines(z, formats, areas, out); break;
    }
    return out;
}

} // namespace xi::client

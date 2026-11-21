// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/currencies.h"

#include <array>
#include <cstring>

namespace xi::ui
{
namespace
{
// Table 0x103862c8, in display order.
constexpr std::array<CurrencyRow, 99> kRows{ {
    { 0, 136, 722 },  { 1, 137, 723 },  { 2, 138, 724 },  { 3, 139, 725 },  { 4, 140, 726 },  { 5, 141, 727 },
    { 6, 142, 728 },  { 7, 143, 729 },  { 8, 144, 730 },  { 9, 145, 731 },  { 10, 146, 732 }, { 90, 473, 1001 },
    { 91, 474, 1002 }, { 92, 475, 1003 }, { 93, 476, 1004 }, { 94, 477, 1005 }, { 95, 478, 1006 }, { 96, 479, 1007 },
    { 97, 480, 1008 }, { 11, 147, 733 }, { 12, 148, 734 }, { 13, 149, 735 }, { 14, 150, 736 }, { 15, 151, 737 },
    { 16, 152, 738 }, { 17, 153, 739 }, { 18, 154, 740 }, { 19, 155, 741 }, { 20, 156, 742 }, { 21, 157, 743 },
    { 22, 158, 744 }, { 23, 159, 745 }, { 24, 160, 746 }, { 25, 161, 747 }, { 26, 162, 748 }, { 27, 163, 749 },
    { 28, 164, 750 }, { 70, 213, 809 }, { 29, 165, 751 }, { 30, 166, 752 }, { 31, 167, 753 }, { 32, 168, 754 },
    { 33, 169, 755 }, { 34, 170, 756 }, { 35, 171, 757 }, { 36, 172, 758 }, { 37, 173, 759 }, { 64, 206, 792 },
    { 38, 174, 760 }, { 63, 205, 791 }, { 39, 175, 761 }, { 40, 176, 762 }, { 41, 177, 763 }, { 42, 178, 764 },
    { 98, 484, 1012 }, { 71, 214, 810 }, { 88, 467, 980 }, { 43, 179, 765 }, { 44, 180, 766 }, { 45, 181, 767 },
    { 46, 182, 768 }, { 47, 183, 769 }, { 48, 184, 770 }, { 49, 185, 771 }, { 50, 186, 772 }, { 65, 207, 793 },
    { 51, 187, 773 }, { 52, 188, 774 }, { 53, 189, 775 }, { 72, 215, 811 }, { 73, 216, 812 }, { 74, 217, 813 },
    { 75, 218, 814 }, { 76, 219, 815 }, { 54, 190, 776 }, { 66, 208, 794 }, { 55, 197, 783 }, { 56, 198, 784 },
    { 57, 199, 785 }, { 58, 200, 786 }, { 59, 201, 787 }, { 60, 202, 788 }, { 61, 203, 789 }, { 62, 204, 790 },
    { 67, 210, 796 }, { 68, 211, 797 }, { 69, 212, 798 }, { 77, 443, 893 }, { 78, 457, 970 }, { 79, 458, 971 },
    { 80, 459, 972 }, { 81, 460, 973 }, { 82, 461, 974 }, { 83, 462, 975 }, { 84, 463, 976 }, { 85, 464, 977 },
    { 86, 465, 978 }, { 87, 466, 979 }, { 89, 472, 1000 },
} };

template <typename T> uint32_t at(std::span<const std::byte> p, std::size_t off)
{
    if (off + sizeof(T) > p.size())
    {
        return 0;
    }
    T v;
    std::memcpy(&v, p.data() + off, sizeof(T));
    return static_cast<uint32_t>(v);
}
} // namespace

std::span<const CurrencyRow> currencyRows()
{
    return kRows;
}

uint32_t currencyValue(uint8_t key, std::span<const std::byte> p)
{
    const std::size_t k = key;
    if (k <= 2) return at<uint32_t>(p, 4 + k * 4);                 // conquest points by nation
    if (k <= 7) return at<uint16_t>(p, 10 + k * 2);                // beastmen's seals .. sacred kindred's crests
    if (k == 8) return at<uint16_t>(p, 0x1a);
    if (k == 9) return at<uint16_t>(p, 0x1c);
    if (k == 10) return at<uint16_t>(p, 0x1e);
    if (k <= 0x13) return at<uint32_t>(p, k * 4 - 0xc);            // guild points
    if (k == 0x14) return at<uint32_t>(p, 0x44);
    if (k <= 0x1c) return at<uint8_t>(p, k + 0x33);                // fewell
    if (k == 0x1d) return at<uint32_t>(p, 0x50);
    if (k == 0x1e) return at<uint32_t>(p, 0x54);
    if (k <= 0x21) return at<uint16_t>(p, 0x1a + k * 2);           // chocobucks
    if (k == 0x22) return at<uint32_t>(p, 0x60);
    if (k <= 0x25) return at<uint8_t>(p, k + 0x41);                // wizened worms
    if (k == 0x26) return at<uint32_t>(p, 0x68);
    if (k <= 0x28) return at<uint16_t>(p, 0x1e + k * 2);           // infamy, prestige
    if (k <= 0x2c) return at<uint32_t>(p, 0x70 + (k - 0x29) * 4);
    if (k <= 0x31) return at<uint32_t>(p, k * 4 - 0x34);           // assault points
    if (k <= 0x36) return at<uint32_t>(p, 0x94 + (k - 0x32) * 4);
    if (k <= 0x39) return at<uint32_t>(p, 0xac + (k - 0x37) * 4);
    if (k <= 0x3e) return at<uint8_t>(p, k + 0x7e);                // echelon trophies
    switch (k)
    {
        case 0x3f: return at<uint8_t>(p, 0xbd);
        case 0x40: return at<uint8_t>(p, 0xcc);
        case 0x41: return at<uint8_t>(p, 0xbe);
        case 0x42: return at<uint8_t>(p, 0xbf);
        case 0x43: return at<uint32_t>(p, 0xc0);
        case 0x44: return at<uint32_t>(p, 0xc4);
        case 0x45: return at<uint32_t>(p, 0xc8);
        case 0x46: return at<uint16_t>(p, 0x5e);
        case 0x47: return at<uint16_t>(p, 0xa8);
        case 0x48: return at<uint32_t>(p, 0xd8) & 0x1ff; // assault plans, 9 bits each
        case 0x49: return (at<uint32_t>(p, 0xd8) >> 9) & 0x1ff;
        case 0x4a: return (at<uint32_t>(p, 0xd8) >> 18) & 0x1ff;
        case 0x4b: return ((at<uint8_t>(p, 0xdc) & 0xf) << 5) | (at<uint32_t>(p, 0xd8) >> 27);
        case 0x4c: return (at<uint16_t>(p, 0xdc) & 0x1ff0) >> 4;
        case 0x4d: return at<uint32_t>(p, 0xe4);
        default: break;
    }
    if (k <= 0x57) return at<uint8_t>(p, k + 0x80);                // Rem's Tale chapters
    switch (k)
    {
        case 0x58: return at<uint16_t>(p, 0xe0);
        case 0x59: return at<uint16_t>(p, 0xaa);
        case 0x5a: return at<uint16_t>(p, 0xe8);                   // crystals stored
        case 0x5b: return at<uint16_t>(p, 0xea);
        case 0x5c: return at<uint16_t>(p, 0xec);
        case 0x5d: return at<uint16_t>(p, 0xee);
        case 0x5e: return at<uint16_t>(p, 0xf0);
        case 0x5f: return at<uint16_t>(p, 0xf2);
        case 0x60: return at<uint16_t>(p, 0xf4);
        case 0x61: return at<uint16_t>(p, 0xf6);
        case 0x62: return at<uint16_t>(p, 0xf8);
        default: return 0;
    }
}
} // namespace xi::ui

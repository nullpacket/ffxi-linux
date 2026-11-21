// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace xi::ui
{
// The Currencies window (ui-format-notes §10.7): "menu evitem" in mode 1, asked for with C2S 0x10F and filled from
// S2C 0x113 CURRENCIES_1. One row per entry of table 0x103862c8 (99 entries of {u32 key, u16 label, u16 help}): the
// label is string #label of file 55653, the help line #help of file 55651, the value the packet field `key` selects.
struct CurrencyRow
{
    uint8_t  key;   // field selector (CODE 0x10229320)
    uint16_t label; // file 55653
    uint16_t help;  // file 55651
};

std::span<const CurrencyRow> currencyRows();

// The value of field `key` in an S2C 0x113 packet (`packet` includes the 4-byte header; CODE 0x10229320). Fields past
// the packet's end read 0.
uint32_t currencyValue(uint8_t key, std::span<const std::byte> packet);
} // namespace xi::ui

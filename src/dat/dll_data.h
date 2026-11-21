// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Tables the client keeps inside FFXiMain.dll. They are located in the user's own DLL by fingerprint (byte sum, then
// SHA-256), never copied into this repository. They sit in the DLL's data section, which the packer leaves as is.
namespace xi::dat
{

std::string sha256Hex(const uint8_t* p, std::size_t n);

// The `size`-byte window of `file` with this fingerprint, if any.
std::optional<std::vector<uint8_t>> findFingerprint(const std::vector<uint8_t>& file, std::size_t size, uint32_t sum, const char* sha);

// Offset of that window in `file`.
std::optional<std::size_t> findFingerprintOffset(const std::vector<uint8_t>& file, std::size_t size, uint32_t sum, const char* sha);

std::vector<uint8_t> readFile(const std::filesystem::path& path);

// "font moji" advance widths, one byte per glyph cell (cell 0 = ' '), 0x440 cells (DLL VA 0x1036c438; §14).
std::optional<std::vector<uint8_t>> mojiWidthsFromDll(const std::filesystem::path& ffxiMainDll);

} // namespace xi::dat

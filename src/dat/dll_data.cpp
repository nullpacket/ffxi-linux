// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/dll_data.h"

#include <fmt/format.h>
#include <openssl/evp.h>

#include <fstream>
#include <stdexcept>

namespace xi::dat
{

std::string sha256Hex(const uint8_t* p, std::size_t n)
{
    unsigned char d[32];
    unsigned int  len = 0;
    EVP_Digest(p, n, d, &len, EVP_sha256(), nullptr);
    std::string s;
    for (unsigned i = 0; i < len; ++i)
    {
        s += fmt::format("{:02x}", d[i]);
    }
    return s;
}

std::optional<std::vector<uint8_t>> findFingerprint(const std::vector<uint8_t>& file, std::size_t size, uint32_t sum, const char* sha)
{
    const auto at = findFingerprintOffset(file, size, sum, sha);
    if (!at)
    {
        return std::nullopt;
    }
    return std::vector<uint8_t>(file.begin() + static_cast<std::ptrdiff_t>(*at), file.begin() + static_cast<std::ptrdiff_t>(*at + size));
}

std::optional<std::size_t> findFingerprintOffset(const std::vector<uint8_t>& file, std::size_t size, uint32_t sum, const char* sha)
{
    if (file.size() < size || size == 0)
    {
        return std::nullopt;
    }
    uint32_t s = 0;
    for (std::size_t i = 0; i < size; ++i)
    {
        s += file[i];
    }
    for (std::size_t i = 0;; ++i)
    {
        if (s == sum && sha256Hex(&file[i], size) == sha)
        {
            return i;
        }
        if (i + size >= file.size())
        {
            return std::nullopt;
        }
        s = s - file[i] + file[i + size];
    }
}

std::vector<uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        throw std::runtime_error(fmt::format("cannot open {}", path.string()));
    }
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

std::optional<std::vector<uint8_t>> mojiWidthsFromDll(const std::filesystem::path& dll)
{
    return findFingerprint(readFile(dll), 0x440, 13682, "c1543abda9a53b6b3c39dac16b476ca017340e50a602f821646eeadf185bafb1");
}

} // namespace xi::dat

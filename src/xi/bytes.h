// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

// Little-endian field access over raw packet buffers. FFXI is x86 little-endian throughout, as are
// the targets we build for, so these are memcpy-based loads/stores with a bounds check.
namespace xi
{

static_assert(std::endian::native == std::endian::little, "ffxi-linux assumes a little-endian host");

template <typename T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] inline T load(std::span<const std::byte> buf, std::size_t off)
{
    if (off + sizeof(T) > buf.size())
    {
        throw std::out_of_range("xi::load past end of buffer");
    }
    T value;
    std::memcpy(&value, buf.data() + off, sizeof(T));
    return value;
}

template <typename T>
    requires std::is_trivially_copyable_v<T>
inline void store(std::span<std::byte> buf, std::size_t off, const T& value)
{
    if (off + sizeof(T) > buf.size())
    {
        throw std::out_of_range("xi::store past end of buffer");
    }
    std::memcpy(buf.data() + off, &value, sizeof(T));
}

inline void storeBytes(std::span<std::byte> buf, std::size_t off, std::span<const std::byte> src)
{
    if (off + src.size() > buf.size())
    {
        throw std::out_of_range("xi::storeBytes past end of buffer");
    }
    std::memcpy(buf.data() + off, src.data(), src.size());
}

// Copies a string into a fixed-width, NUL-padded field (truncating to width - 1 to keep a terminator).
inline void storeString(std::span<std::byte> buf, std::size_t off, std::size_t width, std::string_view s)
{
    if (off + width > buf.size())
    {
        throw std::out_of_range("xi::storeString past end of buffer");
    }
    std::memset(buf.data() + off, 0, width);
    std::memcpy(buf.data() + off, s.data(), std::min(s.size(), width - 1));
}

// Reads a fixed-width, NUL-padded string field.
[[nodiscard]] inline std::string loadString(std::span<const std::byte> buf, std::size_t off, std::size_t width)
{
    if (off + width > buf.size())
    {
        throw std::out_of_range("xi::loadString past end of buffer");
    }
    const auto* p = reinterpret_cast<const char*>(buf.data() + off);
    return std::string(p, strnlen(p, width));
}

[[nodiscard]] inline std::span<const std::byte> asBytes(std::string_view s)
{
    return std::as_bytes(std::span(s.data(), s.size()));
}

std::string hexdump(std::span<const std::byte> buf);

} // namespace xi

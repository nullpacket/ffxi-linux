// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace xi
{

using Md5Digest = std::array<std::byte, 16>;

// Standard MD5 (RFC 1321). FFXI uses it for lobby packet identifiers, map packet checksums and
// blowfish key derivation.
[[nodiscard]] Md5Digest md5(std::span<const std::byte> data);

} // namespace xi

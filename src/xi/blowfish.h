// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace xi
{

// FFXI's Blowfish variant, as spoken by the map server. Two deliberate deviations from textbook
// Blowfish, which must be reproduced bit-exactly:
//   * key bytes are treated as signed chars during the key schedule (bytes >= 0x80 sign-extend);
//   * the round function F is not the standard ((S0+S1)^S2)+S3 — see blowfish.cpp.
// Packets are processed in ECB mode on 8-byte blocks.
class Blowfish
{
public:
    // The 20-byte session key handed through the lobby. Treated as uint32[5] for key increments.
    using SessionKey = std::array<std::byte, 20>;

    explicit Blowfish(const SessionKey& sessionKey);

    // Map server zone change: key[4] (uint32 LE at bytes 16..19) += 2, then re-derive.
    void incrementForZone();

    void encryptBlocks(std::span<std::byte> data) const;
    void decryptBlocks(std::span<std::byte> data) const;

    // Raw schedule from an already-hashed key (exposed for tests).
    void schedule(std::span<const std::byte> key);

    [[nodiscard]] const SessionKey& sessionKey() const
    {
        return key_;
    }

private:
    void derive();
    void encipher(uint32_t& l, uint32_t& r) const;
    void decipher(uint32_t& l, uint32_t& r) const;
    [[nodiscard]] uint32_t f(uint32_t x) const;

    SessionKey                   key_{};
    std::array<uint32_t, 18>     P_{};
    std::array<uint32_t, 4 * 256> S_{};
};

} // namespace xi

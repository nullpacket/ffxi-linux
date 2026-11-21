// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// The last packets in and out of the map session (id, size, time), always kept, for /bugreport (goal 48). Cheap: a
// fixed ring under a mutex; the map client records each sub-packet it sends or handles.
namespace xi
{

class PacketLog
{
public:
    struct Entry
    {
        std::chrono::system_clock::time_point at;
        bool                                  incoming = false;
        uint16_t                              id       = 0;
        uint16_t                              size     = 0;
    };
    static constexpr std::size_t kSize = 256;

    static PacketLog& instance()
    {
        static PacketLog log;
        return log;
    }

    void record(bool incoming, uint16_t id, std::size_t size)
    {
        const std::lock_guard lock(mutex_);
        ring_[next_ % kSize] = { std::chrono::system_clock::now(), incoming, id, static_cast<uint16_t>(size) };
        ++next_;
    }

    // Oldest first, at most `max` entries.
    [[nodiscard]] std::vector<Entry> recent(std::size_t max = kSize) const
    {
        const std::lock_guard lock(mutex_);
        const std::size_t     n = std::min({ max, kSize, next_ });
        std::vector<Entry>    out;
        out.reserve(n);
        for (std::size_t i = next_ - n; i < next_; ++i)
        {
            out.push_back(ring_[i % kSize]);
        }
        return out;
    }

private:
    mutable std::mutex           mutex_;
    std::array<Entry, kSize>     ring_{};
    std::size_t                  next_ = 0;
};

} // namespace xi

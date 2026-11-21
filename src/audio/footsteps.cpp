// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/footsteps.h"

#include <algorithm>

namespace xi::audio
{

std::optional<Footsteps::Step> Footsteps::update(uint32_t id, const Feet& feet, std::span<const dat::Skeleton::Point> points,
                                                 std::span<const glm::mat4> bones, const glm::vec3& at, bool moving, bool walking, float dt,
                                                 const std::function<uint8_t()>& material, bool rain, client::EffectSystem& fx, SoundSystem& sound,
                                                 const SoundSystem::Listener& ear)
{
    if (feet.shoe < 1 || glm::distance(at, ear.position) > 30.0f)
    {
        return std::nullopt;
    }
    // The game steps when a foot point crosses root level; ours stop 0.08-0.11 above it, so this steps at the bottom
    // of each swing instead (INFERRED equivalent): after a lift of 0.12 over the running low, the first frame the
    // point rises again within 0.08 of the low.
    static constexpr std::array<std::size_t, 4> kFeet{ 9, 8, 0xb, 0xa };
    auto& st   = state_[id];
    bool  step = false;
    st.since += dt;
    for (std::size_t f = 0; f < (feet.layout == 2 ? 4u : 2u) && kFeet[f] < points.size(); ++f)
    {
        const auto& pt = points[kFeet[f]];
        if (pt.bone < 0 || static_cast<std::size_t>(pt.bone) >= bones.size())
        {
            continue;
        }
        const float h = (bones[static_cast<std::size_t>(pt.bone)] * glm::vec4(pt.offset[0], pt.offset[1], pt.offset[2], 1.0f)).y - at.y;
        st.low[f]     = std::min(st.low[f] + 0.2f * dt, h);
        if (h > st.low[f] + 0.12f)
        {
            st.down[f] = true; // lifted: the next bottom is a step
        }
        else if (st.down[f] && h > st.prev[f] + 0.002f && st.prev[f] < st.low[f] + 0.08f)
        {
            st.down[f] = false;
            step       = true;
        }
        st.prev[f] = h;
    }
    // A bottom can wobble (two minima 30-60 ms apart); one step per foot fall (INFERRED gap).
    if (!step || st.since <= 0.2f)
    {
        return std::nullopt;
    }
    st.since = 0.0f;
    const uint8_t mat = moving ? material() : 0;
    if (mat == 0 || mat > 9)
    {
        return std::nullopt;
    }
    const int   shoe = feet.shoe, var = std::max<int>(0, feet.variant);
    std::string name{ '0', static_cast<char>('0' + mat), static_cast<char>(shoe < 10 ? '0' + shoe : 'a' + shoe - 10), static_cast<char>('1' + var) };
    const auto  p = sound.playSep(fx.footstepSep(dat::fxName(name), rain), at, 3.0f, 30.0f, ear, false, walking ? 0.8f : 1.0f);
    if (!p)
    {
        return std::nullopt;
    }
    return Step{ std::move(name), mat, *p };
}

} // namespace xi::audio

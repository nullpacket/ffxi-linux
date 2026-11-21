// SPDX-License-Identifier: GPL-3.0-or-later
#include "dat/hd_textures.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

namespace hd = xi::dat::hd;

namespace
{
hd::Image solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    hd::Image img{ w, h, {} };
    for (uint32_t i = 0; i < w * h; ++i)
    {
        img.rgba.insert(img.rgba.end(), { r, g, b, a });
    }
    return img;
}
} // namespace

TEST_CASE("hd textures: the key names the texture and hashes its pixels")
{
    xi::dat::Texture t;
    t.name   = "model   kabe3   ";
    t.width  = 2;
    t.height = 2;
    t.data.assign(16, std::byte{ 7 });
    const auto k = hd::key(t);
    CHECK(k.rfind("model___kabe3-", 0) == 0);
    CHECK(k.size() == std::string("model___kabe3-").size() + 16);
    CHECK(hd::key(t) == k); // stable
    t.data[3] = std::byte{ 8 };
    CHECK(hd::key(t) != k); // changed pixels, new entry
    CHECK(hd::entryPath("/c", k, "_n").string() == "/c/tex/" + k.substr(k.size() - 2) + "/" + k + "_n.png");
}

TEST_CASE("hd textures: cut-out alpha stays two-valued with crisp edges")
{
    auto src = solid(4, 4, 10, 10, 10, 0x80);
    for (uint32_t y = 0; y < 4; ++y)
    {
        src.rgba[(y * 4 + 0) * 4 + 3] = 0; // left column transparent
    }
    CHECK(hd::isCutoutAlpha(src));
    hd::Image dst{ 16, 16, std::vector<uint8_t>(16 * 16 * 4, 0) };
    hd::resizeChannel(src, dst, 3, 1);
    std::set<uint8_t> values;
    for (std::size_t i = 3; i < dst.rgba.size(); i += 4)
    {
        values.insert(dst.rgba[i]);
    }
    CHECK(values == std::set<uint8_t>{ 0, 0x80 });

    auto soft = solid(4, 4, 0, 0, 0, 0);
    for (uint32_t i = 0; i < 16; ++i)
    {
        soft.rgba[i * 4 + 3] = static_cast<uint8_t>(i * 8);
    }
    CHECK_FALSE(hd::isCutoutAlpha(soft)); // gradients resize smoothly
}

TEST_CASE("hd textures: box downscale and the generated maps")
{
    const auto big   = solid(8, 8, 200, 100, 50, 255);
    const auto small = hd::downscale(big, 4);
    CHECK(small.width == 2);
    CHECK(small.height == 2);
    CHECK(small.rgba[0] == 200);
    CHECK(small.rgba[1] == 100);

    const auto n = hd::normalMap(big, 2.0f); // flat colour: straight-up normal
    CHECK(n.rgba[0] == 128);
    CHECK(n.rgba[1] == 128);
    CHECK(n.rgba[2] == 255);
    const auto r = hd::roughnessMap(big);
    CHECK(r.rgba[0] > 150); // flat and mid-dark: fairly rough
}

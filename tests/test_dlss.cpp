// SPDX-License-Identifier: GPL-3.0-or-later
// DLSS helpers (goal 15): mode names, the jitter sequence and the jittered projection. No GPU needed.
#include "gfx/dlss.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <set>
#include <utility>

using Catch::Approx;
using xi::gfx::DlssMode;

TEST_CASE("dlss: mode names", "[dlss]")
{
    CHECK(xi::gfx::parseDlssMode("") == DlssMode::Quality);
    CHECK(xi::gfx::parseDlssMode("quality") == DlssMode::Quality);
    CHECK(xi::gfx::parseDlssMode("balanced") == DlssMode::Balanced);
    CHECK(xi::gfx::parseDlssMode("performance") == DlssMode::Performance);
    CHECK(xi::gfx::parseDlssMode("ultra-performance") == DlssMode::UltraPerformance);
    CHECK(xi::gfx::parseDlssMode("dlaa") == DlssMode::Dlaa);
    CHECK_FALSE(xi::gfx::parseDlssMode("ultra"));
    for (auto m : { DlssMode::Dlaa, DlssMode::Quality, DlssMode::Balanced, DlssMode::Performance, DlssMode::UltraPerformance })
    {
        CHECK(xi::gfx::parseDlssMode(xi::gfx::dlssModeName(m)) == m);
    }
    // NVIDIA's bias: log2(ratio) - 1 (Performance = -2, DLAA = -1).
    CHECK(xi::gfx::dlssLodBias(DlssMode::Performance) == Approx(-2.0f));
    CHECK(xi::gfx::dlssLodBias(DlssMode::Dlaa) == Approx(-1.0f));
}

TEST_CASE("dlss: jitter is centred Halton, distinct over its phases", "[dlss]")
{
    std::set<std::pair<float, float>> seen;
    glm::vec2                         sum{ 0.0f };
    for (uint64_t f = 0; f < 18; ++f) // Quality: 8 x 1.5^2 = 18 phases
    {
        const auto j = xi::gfx::dlssJitter(f, 1.5f);
        CHECK(j.x >= -0.5f);
        CHECK(j.x < 0.5f);
        CHECK(j.y >= -0.5f);
        CHECK(j.y < 0.5f);
        seen.insert({ j.x, j.y });
        sum += j;
    }
    CHECK(seen.size() == 18);
    CHECK(std::abs(sum.x / 18.0f) < 0.05f);
    CHECK(std::abs(sum.y / 18.0f) < 0.05f);
    CHECK(xi::gfx::dlssJitter(0, 1.5f) == xi::gfx::dlssJitter(18, 1.5f)); // the sequence repeats
    CHECK(xi::gfx::dlssJitter(0, 1.0f) == glm::vec2(0.0f, 1.0f / 3.0f - 0.5f)); // Halton index 1: (1/2, 1/3)
}

TEST_CASE("dlss: jittered projection shifts NDC by the offset", "[dlss]")
{
    const glm::mat4 vp = glm::perspective(1.0f, 16.0f / 9.0f, 0.1f, 100.0f) * glm::lookAt(glm::vec3(3, 2, 5), glm::vec3(0), glm::vec3(0, 1, 0));
    const glm::vec2 off{ 0.002f, -0.004f };
    const glm::mat4 vj = xi::gfx::jitterViewProj(vp, off);
    for (const glm::vec3 p : { glm::vec3(0), glm::vec3(1, 0.5f, -2), glm::vec3(-3, 1, 1) })
    {
        const glm::vec4 a = vp * glm::vec4(p, 1.0f), b = vj * glm::vec4(p, 1.0f);
        CHECK(b.x / b.w == Approx(a.x / a.w + off.x).margin(1e-5));
        CHECK(b.y / b.w == Approx(a.y / a.w + off.y).margin(1e-5));
        CHECK(b.z / b.w == Approx(a.z / a.w).margin(1e-6));
    }
}

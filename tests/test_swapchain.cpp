// SPDX-License-Identifier: GPL-3.0-or-later
// When an application-sized surface (Wayland) needs its swapchain rebuilt by hand (#78).
#include "gfx/context.h"

#include <catch2/catch_test_macros.hpp>

using xi::gfx::swapchainStale;

TEST_CASE("swapchain follows the window's size in pixels on application-sized surfaces", "[gfx]")
{
    const VkExtent2D swap{ 2080, 1170 }; // 1600x900 on a 1.3 output
    CHECK_FALSE(swapchainStale(true, swap, 2080, 1170));
    CHECK(swapchainStale(true, swap, 1600, 900));  // moved to a 1.0 output (the #78 report)
    CHECK(swapchainStale(true, swap, 2600, 1462)); // resized
    CHECK_FALSE(swapchainStale(true, swap, 0, 0)); // minimised: keep the old one
}

TEST_CASE("surfaces that size the swapchain themselves report out-of-date instead", "[gfx]")
{
    CHECK_FALSE(swapchainStale(false, VkExtent2D{ 2080, 1170 }, 1600, 900));
}

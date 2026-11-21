// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/status_pulse.h"

#include <catch2/catch_test_macros.hpp>

// Status icons wearing off (CODE 0x10227ad0 / 0x10227750; docs/ui-format-notes.md §9.4).
using namespace xi::ui;

TEST_CASE("the pulse is a 40-tick triangle between half and full brightness", "[status-pulse]")
{
    CHECK(statusPulseLevel(40.0f) == 0x40); // the counter's start: dim
    CHECK(statusPulseLevel(30.0f) == 0x60);
    CHECK(statusPulseLevel(20.0f) == 0x80); // the middle: full
    CHECK(statusPulseLevel(10.0f) == 0x60);
    CHECK(statusPulseLevel(0.0f) == 0x40);
    CHECK(statusPulseLevel(5.0f) == 0x50);
    CHECK(statusPulseLevel(1.0f) == 0x43); // 0.05 * 64 = 3.2, truncated
    for (int i = 0; i <= 40; ++i)
    {
        const auto v = statusPulseLevel(static_cast<float>(i));
        CHECK(v >= 0x40);
        CHECK(v <= 0x80);
    }
}

TEST_CASE("the counter runs 40 down to 0 and starts again", "[status-pulse]")
{
    CHECK(statusPulseCounter(0.0) == 40.0f);
    CHECK(statusPulseCounter(10.0) == 30.0f);
    CHECK(statusPulseCounter(39.0) == 1.0f);
    CHECK(statusPulseCounter(40.0) == 40.0f);
    CHECK(statusPulseCounter(4020.0) == 20.0f);
}

TEST_CASE("the pulse starts at 15 seconds left, counted up to whole seconds", "[status-pulse]")
{
    CHECK(statusSecondsLeftCeil(15 * 60) == 15);
    CHECK(statusSecondsLeftCeil(15 * 60 + 1) == 16); // 15 s and a tick: not yet
    CHECK(statusSecondsLeftCeil(1) == 1);
    CHECK(statusSecondsLeftCeil(0) == 0);
    CHECK(statusSecondsLeftCeil(16 * 60) > kStatusPulseSeconds);
    CHECK(statusSecondsLeftCeil(14 * 60 + 30) <= kStatusPulseSeconds);
}

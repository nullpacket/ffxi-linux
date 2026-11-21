// SPDX-License-Identifier: GPL-3.0-or-later
#include "client/session_end.h"

#include <catch2/catch_test_macros.hpp>

using xi::client::SessionEnd;
using xi::client::sessionEndNotice;

namespace
{
SessionEnd ended(std::string_view status, bool lost = false)
{
    SessionEnd e;
    e.finished       = true;
    e.connectionLost = lost;
    e.status         = status;
    return e;
}
} // namespace

TEST_CASE("session end: a running session or an asked-for end shows no notice", "[session_end]")
{
    auto e     = ended("lost connection to the map server", true);
    e.finished = false;
    CHECK_FALSE(sessionEndNotice(e));
    e          = ended("lost connection to the map server", true);
    e.quitting = true;
    CHECK_FALSE(sessionEndNotice(e));
    e                   = ended("logged out");
    e.loggedOutByServer = true;
    CHECK_FALSE(sessionEndNotice(e));
}

TEST_CASE("session end: the map server going silent or a failed login goes back to the login form", "[session_end]")
{
    CHECK(sessionEndNotice(ended("lost connection to the map server", true)) == "Lost connection to the server.");
    CHECK(sessionEndNotice(ended("error: map: no reply to 0x00A login")) == "Could not connect: map: no reply to 0x00A login");
    CHECK(sessionEndNotice(ended("logged out")) == "Disconnected from the server.");
}

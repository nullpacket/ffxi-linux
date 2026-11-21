// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// What the player is told when a session ends on its own (#43): the map server stopped answering (LSB's watchdog kills
// and restarts it) or the login failed. The client goes back to the login form with this line instead of freezing.
#include <optional>
#include <string>
#include <string_view>

namespace xi::client
{

struct SessionEnd
{
    bool             finished          = false; // the session thread has returned
    bool             quitting          = false; // the player asked to quit (the window closes)
    bool             loggedOutByServer = false; // a /logout or /shutdown countdown ran out (the window closes)
    bool             connectionLost    = false; // no reply from the map server for the timeout
    std::string_view status;                    // the session's last status ("error: ..." on a failure)
};

// The login form's notice for an unexpected end, or nullopt when the session is running or ended as asked.
[[nodiscard]] inline std::optional<std::string> sessionEndNotice(const SessionEnd& e)
{
    if (!e.finished || e.quitting || e.loggedOutByServer)
    {
        return std::nullopt;
    }
    if (e.connectionLost)
    {
        return std::string("Lost connection to the server.");
    }
    constexpr std::string_view kError = "error: ";
    if (e.status.starts_with(kError))
    {
        return "Could not connect: " + std::string(e.status.substr(kError.size()));
    }
    return std::string("Disconnected from the server.");
}

} // namespace xi::client

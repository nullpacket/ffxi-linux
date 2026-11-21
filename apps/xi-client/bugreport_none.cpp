// SPDX-License-Identifier: GPL-3.0-or-later
// /bugreport without XI_PRIVATE_FEATURES (the public build): the command is parsed as
// usual and answers that it is not built in; the bugreport_* settings keys are still read and ignored.
#include "client_app.h"

#include <spdlog/spdlog.h>

namespace xi::app
{

void installBugReportLog() {}

void ClientApp::stepBugReport()
{
    if (bugReportRequest)
    {
        bugReportRequest.reset();
        spdlog::info("bugreport: not built in (XI_PRIVATE_FEATURES=OFF)");
        if (session)
        {
            session->note("/bugreport is not built into this client.");
        }
    }
}

} // namespace xi::app

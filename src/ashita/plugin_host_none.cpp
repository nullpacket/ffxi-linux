// SPDX-License-Identifier: GPL-3.0-or-later
// Ashita plugins (issue #34): the stand-in when the client is built without the Ashita SDK headers (or Wine's Win32
// headers); `/load` reports pluginSupportNote().
#include "ashita/plugin_host.h"

namespace xi::ashita
{

std::unique_ptr<PluginHost> makePluginHost(AddonManager&)
{
    return nullptr;
}

const char* pluginSupportNote()
{
    return "this build has no plugin support (it needs the Ashita SDK headers and Wine's Win32 headers at build time; "
           "docs/ashita-compat.md \"Plugins\").";
}

} // namespace xi::ashita

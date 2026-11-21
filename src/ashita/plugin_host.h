// SPDX-License-Identifier: GPL-3.0-or-later
// Ashita plugins built natively for Linux (issue #34, docs/ashita-compat.md "Plugins"): the plugin side of the Ashita
// layer. A plugin is an open-source Ashita v4 plugin compiled from its own source against the Ashita SDK headers into
// `<name>.so` (tools/ashita_sdk_port.py, cmake/ashita_plugins.cmake); the host loads it with dlopen, hands it an
// IAshitaCore that answers from the client's state, and passes it the commands and packets Ashita would.
//
// This header does not need the SDK: the client is built with plugin support only when the SDK headers (and Wine's
// Win32 headers, for their types) are found; otherwise makePluginHost answers nullptr and `/load` says why.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace xi::ashita
{

class AddonManager;
struct PacketEvent;

class PluginHost
{
public:
    virtual ~PluginHost() = default;

    // /load NAME [ARGS]: finds `NAME.so` (any case) in the plugin folders and starts it. False with a reason in `error`.
    virtual bool load(const std::string& name, const std::string& args, std::string& error) = 0;
    virtual bool unload(const std::string& name)                                            = 0;
    virtual void unloadAll()                                                                = 0;
    [[nodiscard]] virtual std::vector<std::string> loaded() const                           = 0;
    [[nodiscard]] virtual bool                     isLoaded(const std::string& name) const  = 0;

    // A command line before the addons see it (render thread, manager lock held). A plugin may rewrite `line` in place
    // (as Ashita lets it); true when one handled it.
    virtual bool command(std::string& line, int mode, bool injected) = 0;
    // A packet before the addons see it (session thread, lock held); plugins may change e.data or block it.
    virtual void packet(bool incoming, PacketEvent& e) = 0;
    // Once a frame after the manager's state is updated.
    virtual void frame() = 0;
};

// Where `/load` looks, in order: <ashita root>/plugins, then the client's own plugins folder (next to the executable,
// where the build puts the plugins it compiles).
[[nodiscard]] std::vector<std::filesystem::path> pluginFolders(const std::filesystem::path& root);

// nullptr when the client was built without the Ashita SDK.
[[nodiscard]] std::unique_ptr<PluginHost> makePluginHost(AddonManager& manager);
// Why plugins cannot load in this build ("" when they can).
[[nodiscard]] const char* pluginSupportNote();

} // namespace xi::ashita

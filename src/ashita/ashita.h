// SPDX-License-Identifier: GPL-3.0-or-later
// Ashita v4 compatibility: runs Ashita addons (Lua) unchanged against the client's own state (PLANS goal 31,
// docs/ashita-compat.md). Each addon gets its own LuaJIT state with the `ashita` / `AshitaCore` API the Ashita
// addon loader provides, built by an embedded prelude (prelude.lua) over a small native module.
#pragma once

#include "client/session.h"

#include <cstddef>
#include <deque>
#include <unordered_map>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct lua_State;

namespace xi::ashita
{
class PluginHost;
} // namespace xi::ashita

namespace xi::dat
{
class ItemTable;
class SpellTable;
class AbilityTable;
class BattleText;
class StringTable;
} // namespace xi::dat

namespace xi::ashita
{

// The two Ashita releases the layer answers as (docs/ashita-compat.md): the latest upstream release and the one
// HorizonXI ships. The Lua API is the same; they differ in the version strings addons can read, the libs each ships
// (HorizonXI adds bitmap / color / lin / typedefs) and the RaiseEvent overloads of the plugin managers.
enum class Profile
{
    Latest,    // Ashita v4.3.2.1 (2026-09-11), addons plugin 4.20
    HorizonXI, // Ashita v4.3.1.2 (HorizonXI launcher 1.9.0)
};
[[nodiscard]] const char* profileVersion(Profile p);
[[nodiscard]] std::optional<Profile> parseProfile(std::string_view s); // "latest" / "horizonxi" / a version string
// "auto": HorizonXI when the addons folder has HorizonXI's extra libs (libs/typedefs, libs/lin), else Latest.
[[nodiscard]] Profile detectProfile(const std::filesystem::path& root);

// Game data the resource manager reads (all optional).
struct Resources
{
    std::shared_ptr<const dat::ItemTable> items;
    const dat::SpellTable*                spells    = nullptr;
    const dat::AbilityTable*              abilities = nullptr;
    std::shared_ptr<const dat::BattleText> text;      // spell / ability / status names
    const dat::StringTable*               zoneNames = nullptr;
};

// What the client hands over each frame (render thread).
struct Frame
{
    std::shared_ptr<const client::WorldSnapshot> world;
    client::Entity self;          // our own entity (position, look) as drawn
    uint16_t       targetIndex = 0, subTargetIndex = 0;
    bool           subTargetActive = false;
    int            windowW = 0, windowH = 0; // the window, pixels
    int            menuW = 512, menuH = 512;  // the game's menu resolution (Config / client.ini)
};

// The client side the addons act on. Called with the manager's lock held, on the render thread (commands and chat are
// buffered when they come from the session thread).
struct Host
{
    std::function<void(const std::string& line, int mode)>               command;  // as typed / from a macro
    std::function<void(const std::string& text, int mode)>               chat;     // a line in the log
    std::function<void(uint16_t id, std::vector<std::byte> payload)>     send;     // C2S, payload after the header
    std::function<void(std::vector<std::byte> packet)>                   inject;   // S2C, full packet
    std::function<void(uint16_t index)>                                  target;   // ITarget:SetTarget
};

// One packet as the packet events see it.
struct PacketEvent
{
    uint16_t               id = 0;
    std::vector<std::byte> data;     // full packet with its 4-byte header; addons may change it
    std::span<const std::byte> chunk; // the whole datagram body it came in (incoming) / goes out in
    bool                   injected = false;
    bool                   blocked  = false;
};

class AddonManager
{
public:
    // `root` is the Ashita install the addons expect (addons/, config/, scripts/); GetInstallPath answers it.
    AddonManager(std::filesystem::path root, Profile profile, Resources res, Host host);
    ~AddonManager();
    AddonManager(const AddonManager&)            = delete;
    AddonManager& operator=(const AddonManager&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const
    {
        return root_;
    }
    [[nodiscard]] Profile profile() const
    {
        return profile_;
    }
    // The state of the current frame and the game data (for the native module; render / session thread with the lock).
    [[nodiscard]] const Frame& frame() const
    {
        return frame_;
    }
    [[nodiscard]] uint64_t frameNumber() const
    {
        return frameNo_;
    }
    [[nodiscard]] const Resources& resources() const
    {
        return res_;
    }

    bool load(const std::string& name);   // /addon load
    bool unload(const std::string& name); // /addon unload
    void unloadAll();
    [[nodiscard]] std::vector<std::string> loaded() const;
    // The last errors an addon reported (a failed load, or an error in one of its callbacks), oldest first, at most 20;
    // kept after it unloads, for the Config menu's Add-ons page. clearErrors forgets them (a fresh load or reload).
    [[nodiscard]] std::vector<std::string> errors(const std::string& name) const;
    void clearErrors(const std::string& name);
    void noteError(const std::string& name, std::string message);
    // Runs the boot script's `/addon load` lines (scripts/default.txt, or `script` if given); other lines are skipped.
    void runBootScript(const std::filesystem::path& script = {});

    // Render thread, once a frame before the interface draws: new state, buffered commands / chat, tasks, text_in for new
    // log lines.
    void update(Frame frame);
    // Inside the ImGui frame: d3d_present, then the addons' font objects and primitives.
    void present();

    // A chat line about to run (typed: mode 1, macro 2). True when a plugin, an addon (or /addon, /load itself) took it.
    bool command(const std::string& line, int mode);
    // The same; a plugin may rewrite `line` in place (Shorthand strips its "/raw " prefix), and the rest of the chain and
    // the game then see the new text.
    bool commandEdit(std::string& line, int mode);
    // Natively built Ashita plugins (issue #34): /load, /unload, the boot script's /load lines.
    bool                                   loadPlugin(const std::string& name, const std::string& args = {});
    bool                                   unloadPlugin(const std::string& name);
    [[nodiscard]] std::vector<std::string> loadedPlugins() const;
    // A line about to be said (text_out); may be changed. True when blocked.
    bool textOut(std::string& line, int mode);
    // Packet events (session thread). Return false when blocked.
    bool packetIn(PacketEvent& e);
    bool packetOut(PacketEvent& e);
    // Mouse / keyboard (render thread): true when an addon blocked it. `message` is the Win32 message (WM_MOUSEMOVE ...),
    // x / y in window coordinates (the addons' draw space), delta the wheel's (120 a notch); `vk` a Windows virtual key
    // (vkFromSdl). Shift held (VK_SHIFT through key()) lets Shift + left drag move unlocked font / primitive objects.
    bool mouse(uint32_t message, int x, int y, int delta);
    bool key(uint32_t vk, bool down, bool repeat);

    // Log lines the addons changed or blocked (text_in), by ChatLine::serial; the log reads these.
    struct LineEdit
    {
        bool        hidden = false;
        std::string text; // the whole line as shown (speaker included)
    };
    [[nodiscard]] std::optional<LineEdit> lineEdit(uint64_t serial) const;

    struct Addon
    {
        std::string           name;
        std::filesystem::path dir;
        lua_State*            L      = nullptr;
        bool                  loaded = false;
    };

private:
    friend struct Native;
    friend struct PluginBridge;
    std::filesystem::path root_;
    Profile               profile_;
    Resources             res_;
    Host                  host_;

    mutable std::recursive_mutex                m_;
    std::vector<std::unique_ptr<Addon>>         addons_;
    Frame                                       frame_;
    uint64_t                                    frameNo_   = 0;
    uint64_t                                    chatSeen_  = 0; // newest ChatLine::serial given to text_in
    bool                                        chatPrimed_ = false; // the log as it was at the first frame is skipped
    std::vector<std::pair<std::string, int>>    pendingCommands_;
    std::vector<std::pair<std::string, int>>    pendingChat_;
    std::vector<std::string>                    injectedLines_; // lines we queued, seen again as injected commands
    struct Printed
    {
        std::string text;
        bool        error = false;
    };
    std::deque<Printed>                         printed_; // log lines addons wrote, waiting to be seen in the log
    std::unordered_map<std::string, std::deque<std::string>> errors_; // by lower-case addon name
public:
    static constexpr int kErrorMode = -99; // __xi.chat mode of an addon's error report
private:
    std::unordered_map<uint64_t, LineEdit>      edits_;
    std::deque<uint64_t>                        editOrder_;
    bool                                        inUpdate_ = false;
    bool                                        shift_    = false; // VK_SHIFT down (key events)
    std::unique_ptr<PluginHost>                 plugins_;          // nullptr in a build without the Ashita SDK

    void say(const std::string& text, int mode = 1);
    void flush();
    bool addonCommand(const std::string& line);
    bool pluginCommand(const std::string& line);
    void textIn(const client::ChatLine& line, bool injected);
    bool packet(bool incoming, PacketEvent& e);
};

} // namespace xi::ashita

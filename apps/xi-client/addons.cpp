// SPDX-License-Identifier: GPL-3.0-or-later
// Ashita addons in the client (PLANS goal 31, docs/ashita-compat.md): the AddonManager's host side and its frame.
#include "client_app.h"

#include "xi/ashita_dir.h"

#include "ashita/ashita.h"
#include "ashita/lua_state.h"
#include "ashita/scan_registry.h"

#include <cstdlib>

namespace xi::app
{

namespace
{
// Chat text from an addon: FFXI colour codes (0x1E / 0x1F and a byte) dropped, one log line per line break.
std::vector<std::string> plainLines(const std::string& s)
{
    std::vector<std::string> out(1);
    for (std::size_t k = 0; k < s.size(); ++k)
    {
        const char c = s[k];
        if (c == '\x1E' || c == '\x1F')
        {
            ++k;
        }
        else if (c == '\n' || c == '\x07')
        {
            out.emplace_back();
        }
        else if (c != '\r')
        {
            out.back().push_back(c);
        }
    }
    std::erase_if(out, [](const std::string& l) { return l.empty(); });
    return out;
}

std::string env(const char* k)
{
    const char* v = std::getenv(k);
    return v ? v : "";
}
} // namespace

// The Ashita folder: XI_ASHITA_DIR, else client.ini [ffxi-linux] ashita_dir, else the game folder itself (issue #73:
// <game>/addons, created on first run with addons/libs, config/addons and scripts; xi/ashita_dir.h).
std::filesystem::path ClientApp::ashitaRoot() const
{
    return xi::ashitadir::effectiveRoot(args.display_.ashitaDir, args.root);
}

// The addons in ashitaRoot(). The profile (which Ashita release to answer as): ashita_version / XI_ASHITA_VERSION =
// auto (default) / latest / horizonxi. Addons to load: ashita_load / XI_ASHITA_LOAD (comma-separated), then the
// `/addon load` lines of ashita_script / XI_ASHITA_SCRIPT.
void ClientApp::startAddons()
{
    const auto root = ashitaRoot();
    if (root.empty())
    {
        return;
    }
    if (const auto err = xi::ashitadir::ensureLayout(root); !err.empty())
    {
        spdlog::warn("ashita: {}; addons are off", err);
        return;
    }
    // The bundled addons and libraries (#138): whatever the folder doesn't have yet, never overwriting the user's.
    if (const auto bundle = xi::ashitadir::bundledDir(); !bundle.empty())
    {
        try
        {
            const auto r = xi::ashitadir::installBundled(bundle, root);
            if (r.libFiles > 0 || !r.addons.empty())
            {
                spdlog::info("ashita: installed {} bundled library files and {} bundled addons into {}", r.libFiles,
                             r.addons.size(), (root / "addons").string());
            }
        }
        catch (const std::exception& e)
        {
            spdlog::warn("ashita: installing the bundled addons failed: {}", e.what());
        }
    }
    // Window positions and sizes the addons' ImGui windows were left at, in the file Ashita keeps them in (#111).
    imguiIni_.use(ashita::imguiIniPath(root));
    if (!xi::ashitadir::libsPresent(root))
    {
        spdlog::warn("ashita: {}", xi::ashitadir::libsMissingHint(root));
    }
    const std::string dir = root.string();
    const std::string ver     = !env("XI_ASHITA_VERSION").empty() ? env("XI_ASHITA_VERSION") : args.display_.ashitaVersion;
    const auto        profile = (ver.empty() || ver == "auto") ? ashita::detectProfile(dir) : ashita::parseProfile(ver).value_or(ashita::Profile::Latest);

    ashita::setGameData(&index); // mob ability names, status icon bitmaps
    // The memory-signature stand-ins (scan_registry.h): the game DLL to map, and Config values by id.
    ashita::setGameImage(std::filesystem::path(args.root) / "FFXiMain.dll");
    ashita::ConfigHooks cfgHooks;
    cfgHooks.get = [this](int32_t id) -> std::optional<int32_t> {
        const auto it = args.display_.gameConfig.find(id);
        return it != args.display_.gameConfig.end() ? it->second : ui::GameUi::cfgDefault(id);
    };
    cfgHooks.set = [this](int32_t id, int32_t value) {
        args.display_.gameConfig[id] = value; // the value the Config pages show; a few act only from the menu
        return true;
    };
    cfgHooks.entry = [this](int32_t id) -> std::optional<ashita::ConfigEntry> {
        ashita::ConfigEntry e;
        const auto it = args.display_.gameConfig.find(id);
        e.def   = ui::GameUi::cfgDefault(id);
        e.value = it != args.display_.gameConfig.end() ? it->second : e.def;
        return e;
    };
    ashita::setConfigHooks(std::move(cfgHooks));
    startAddonNatives();
    ashita::Resources res;
    res.items     = itemTable;
    res.spells    = spellData ? &*spellData : nullptr;
    res.abilities = abilityData ? &*abilityData : nullptr;
    res.text      = battleText;
    res.zoneNames = areaNames ? &*areaNames : nullptr;

    ashita::Host hooks;
    hooks.command = [this](const std::string& line, int mode) { gameUi.runLine(line, mode); };
    hooks.chat    = [this](const std::string& text, int) {
        for (auto& l : plainLines(text))
        {
            if (session)
            {
                session->note(std::move(l));
            }
            else
            {
                spdlog::info("ashita: {}", l);
            }
        }
    };
    hooks.send = [this](uint16_t id, std::vector<std::byte> payload) {
        if (session)
        {
            session->sendInjected(id, std::move(payload));
        }
    };
    hooks.inject = [this](std::vector<std::byte> packet) {
        if (session)
        {
            session->injectIncoming(std::move(packet));
        }
    };
    hooks.target = [this](uint16_t actIndex) {
        for (const auto& e : world.entities)
        {
            if (e.index == actIndex)
            {
                targetId = e.id;
            }
        }
    };
    addons = std::make_unique<ashita::AddonManager>(dir, profile, std::move(res), std::move(hooks));
    updateAddons(); // the window size before any addon loads (libs/scaling.lua reads it at load)
    addonsLive = addons.get();

    std::string list = !env("XI_ASHITA_LOAD").empty() ? env("XI_ASHITA_LOAD") : args.display_.ashitaLoad;
    for (std::size_t i = 0; i < list.size();)
    {
        auto j = list.find(',', i);
        auto n = list.substr(i, j == std::string::npos ? std::string::npos : j - i);
        std::erase(n, ' ');
        if (!n.empty())
        {
            addons->load(n);
        }
        i = j == std::string::npos ? list.size() : j + 1;
    }
    const std::string bootScript = !env("XI_ASHITA_SCRIPT").empty() ? env("XI_ASHITA_SCRIPT") : args.display_.ashitaScript;
    if (!bootScript.empty())
    {
        addons->runBootScript(bootScript);
    }
}

// Config -> Add-ons picked another Ashita release: saved to client.ini (ashita_version), and the addons start again
// under it with the same ones loaded.
void ClientApp::restartAddons(const std::string& release)
{
    args.display_.ashitaVersion = release;
    if (const auto file = DisplaySettings::userFile(); file && !DisplaySettings::saveOwnText(*file, "ashita_version", release))
    {
        spdlog::warn("addons: could not save ashita_version to {}", file->string());
    }
    if (!env("XI_ASHITA_VERSION").empty())
    {
        spdlog::warn("addons: XI_ASHITA_VERSION is set and wins over the saved ashita_version");
    }
    const auto keep = addons ? addons->loaded() : std::vector<std::string>{};
    addonsLive      = nullptr;
    addons.reset();
    startAddons();
    if (addons)
    {
        for (const auto& n : keep)
        {
            if (const auto l = addons->loaded(); std::find(l.begin(), l.end(), n) == l.end())
            {
                addons->load(n);
            }
        }
    }
}

// Once a frame, after targeting: the state the addons read.
void ClientApp::updateAddons()
{
    if (!addons)
    {
        return;
    }
    ashita::Frame f;
    f.world = std::make_shared<const client::WorldSnapshot>(world);
    f.self  = selfEntity;
    f.self.pos = gfx::ZoneScene::renderToGame(feet);
    for (const auto& e : world.entities)
    {
        if (e.id == targetId)
        {
            f.targetIndex = e.index;
        }
    }
    if (targetId == world.charId && world.charId != 0)
    {
        f.targetIndex = world.actIndex;
    }
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    f.windowW = w;
    f.windowH = h;
    f.menuW   = args.display_.menuWidth > 0 ? args.display_.menuWidth : w * 2 / 3;
    f.menuH   = args.display_.menuHeight > 0 ? args.display_.menuHeight : h * 2 / 3;
    ashita::ClientUi ui;
    ui.topMenu         = gameUi.topLayout();
    ui.eventActive     = cutscene && cutscene->active();
    ui.interfaceHidden = gameUi.windowsHidden();
    ui.chatExpanded    = gameUi.logFull();
    ui.eventMessage    = cutscene && cutscene->waitingMessage();
    ashita::setClientUi(std::move(ui));
    addons->update(std::move(f));
}

} // namespace xi::app

// SPDX-License-Identifier: GPL-3.0-or-later
// Ashita addons' native settings (src/ashita/scan_registry.h NativeHooks / MacroHooks; docs/ashita-compat.md "Native
// settings"): the client settings an addon's patch of the game's memory stands for. Each defaults to the client's own
// behaviour; any name not handled here answers "unsupported", so its signature fails as a missing pattern would.
#include "client_app.h"

#include "ashita/scan_registry.h"

#include <cstring>
#include <optional>
#include <utility>

namespace xi::app
{

namespace
{
// "name_colour.N" -> N (0..22), else -1.
int nameColourSlot(std::string_view name)
{
    constexpr std::string_view kPrefix = "name_colour.";
    if (!name.starts_with(kPrefix))
    {
        return -1;
    }
    int n = 0;
    for (const char c : name.substr(kPrefix.size()))
    {
        if (c < '0' || c > '9')
        {
            return -1;
        }
        n = n * 10 + (c - '0');
    }
    return n <= 22 ? n : -1;
}

// "log.windowN.<field>" (N 1 or 2) -> the window (0 / 1) and the field, else -1.
std::pair<int, std::string_view> logObjectField(std::string_view name)
{
    constexpr std::string_view kPrefix = "log.window";
    if (!name.starts_with(kPrefix) || name.size() < kPrefix.size() + 3 || name[kPrefix.size() + 1] != '.')
    {
        return { -1, {} };
    }
    const char n = name[kPrefix.size()];
    if (n != '1' && n != '2')
    {
        return { -1, {} };
    }
    return { n - '1', name.substr(kPrefix.size() + 2) };
}

} // namespace

void ClientApp::startAddonNatives()
{
    ashita::NativeHooks h;
    h.supports = [](std::string_view name) {
        static constexpr std::string_view kNames[] = {
            "interface.hidden", "event.message.waiting", "event.message.key", "chat.instant", "macro.instant",
            "draw_distance.world", "draw_distance.entity", "aspect.factor", "clear_colour", "fps.divisor",
            "window.party.visible", "window.target.visible", "window.alliance1.visible", "window.alliance2.visible",
            "window.macro_palette.visible", "chat.filter.0", "chat.filter.1", "chat.filter.2",
            "camera.distance.min", "camera.distance.max", "camera.battle_distance.min", "camera.battle_distance.max",
            "camera.pan.horizontal", "camera.pan.vertical", "camera.battle_range", "map.all", "map.all.view",
        };
        static constexpr std::string_view kLogFields[] = { "height",    "lines",    "max_lines",
                                                           "min_lines", "text_top", "shown" };
        if (const auto [win, field] = logObjectField(name); win >= 0)
        {
            return std::ranges::find(kLogFields, field) != std::end(kLogFields);
        }
        return nameColourSlot(name) >= 0 || std::ranges::find(kNames, name) != std::end(kNames);
    };
    h.get = [this](std::string_view name) -> std::optional<double> {
        auto& tune  = moveState_.gameCam.tuning;
        auto& shown = gameUi.windowsShown;
        if (name == "interface.hidden")
        {
            return gameUi.windowsHidden() ? 1.0 : 0.0;
        }
        if (const auto [win, field] = logObjectField(name); win >= 0)
        {
            // The log objects (ui/log_object.h): the client's own shown height, what is drawn, its line limits and how
            // far its text top is above the screen's bottom.
            const auto& o = gameUi.logObject(win);
            if (field == "height")
            {
                return o.height;
            }
            if (field == "lines")
            {
                return o.lines;
            }
            if (field == "max_lines")
            {
                return o.maxLines;
            }
            if (field == "min_lines")
            {
                return o.minLines;
            }
            if (field == "text_top")
            {
                return o.textTop;
            }
            if (field == "shown")
            {
                return o.shown ? 1.0 : 0.0;
            }
            return std::nullopt;
        }
        if (name == "event.message.waiting")
        {
            return cutscene && cutscene->waitingMessage() ? 1.0 : 0.0;
        }
        if (name == "chat.instant" || name == "macro.instant")
        {
            return 1.0; // the log shows a line when it arrives; the palette shows when Ctrl / Alt goes down
        }
        if (name == "draw_distance.world")
        {
            return addonOverrides_.drawWorld;
        }
        if (name == "draw_distance.entity")
        {
            return addonOverrides_.drawEntity;
        }
        if (name == "aspect.factor")
        {
            // The camera's +0x2F0 as the game sets it: y / (0.75 x) of the shape in use.
            int w = 0, hh = 0;
            SDL_GetWindowSize(window, &w, &hh);
            const float shape = gfx::configuredAspect() > 0.0f ? gfx::configuredAspect() : (hh > 0 ? static_cast<float>(w) / static_cast<float>(hh) : 4.0f / 3.0f);
            return gfx::aspectFactorOverride() > 0.0f ? gfx::aspectFactorOverride() : 1.0f / (0.75f * shape);
        }
        if (name == "clear_colour")
        {
            return addonOverrides_.clearColour ? std::optional<double>(*addonOverrides_.clearColour) : std::nullopt;
        }
        if (name == "fps.divisor")
        {
            return addonOverrides_.fpsDivisor > 0 ? std::optional<double>(addonOverrides_.fpsDivisor) : std::nullopt;
        }
        if (const int i = nameColourSlot(name); i >= 0)
        {
            if (const auto& o = ui::nameColourOverrides()[static_cast<std::size_t>(i)])
            {
                return *o;
            }
            return std::nullopt; // the game's default for the table (0x80808080)
        }
        if (name == "window.party.visible")
        {
            return shown.party ? 1.0 : 0.0;
        }
        if (name == "window.target.visible")
        {
            return shown.target ? 1.0 : 0.0;
        }
        if (name == "window.alliance1.visible")
        {
            return shown.alliance1 ? 1.0 : 0.0;
        }
        if (name == "window.alliance2.visible")
        {
            return shown.alliance2 ? 1.0 : 0.0;
        }
        if (name == "window.macro_palette.visible")
        {
            return shown.macroPalette ? 1.0 : 0.0;
        }
        if (name.starts_with("chat.filter."))
        {
            // S2C 0x0B4's SAVE_CONF head: flags, chat filters 1 / 2 (u32 each).
            if (!world.saveConfValid)
            {
                return std::nullopt;
            }
            const std::size_t k = static_cast<std::size_t>(name.back() - '0');
            uint32_t          v = 0;
            std::memcpy(&v, world.saveConf.data() + 4 * k, 4);
            return v;
        }
        if (name == "camera.distance.min")
        {
            return tune.nearDist;
        }
        if (name == "camera.distance.max")
        {
            return tune.follow;
        }
        if (name == "camera.battle_distance.min")
        {
            return tune.lockNear;
        }
        if (name == "camera.battle_distance.max")
        {
            return tune.lockFar;
        }
        if (name == "camera.pan.horizontal")
        {
            return tune.yawPerTick;
        }
        if (name == "camera.pan.vertical")
        {
            return tune.risePerTick;
        }
        if (name == "camera.battle_range")
        {
            return tune.lockCone;
        }
        if (name == "map.all")
        {
            return gameUi.allMaps.map ? 1.0 : 0.0;
        }
        if (name == "map.all.view")
        {
            return gameUi.allMaps.view ? 1.0 : 0.0;
        }
        return std::nullopt;
    };
    h.set = [this](std::string_view name, double value) {
        if (const auto [win, field] = logObjectField(name); win >= 0)
        {
            // An addon's write of a log object's +0x34 (each frame for fancychat, so not logged); -1: it unloaded.
            if (field == "height")
            {
                gameUi.addonLogHeight(win, value < 0 ? std::nullopt : std::optional<int>(static_cast<int>(value)));
            }
            return;
        }
        spdlog::info("ashita native: {} = {}", name, value);
        const auto f     = static_cast<float>(value);
        auto&      tune  = moveState_.gameCam.tuning;
        auto&      shown = gameUi.windowsShown;
        if (name == "interface.hidden" && (value != 0.0) != gameUi.windowsHidden())
        {
            gameUi.toggleHidden();
        }
        else if (name == "event.message.key" && static_cast<int>(value) == 5) // Enter on the waiting message
        {
            eventKey = true;
        }
        else if (name == "draw_distance.world" && f > 0.0f)
        {
            addonOverrides_.drawWorld = f;
        }
        else if (name == "draw_distance.entity" && f > 0.0f)
        {
            addonOverrides_.drawEntity = f;
        }
        else if (name == "aspect.factor")
        {
            gfx::aspectFactorOverride() = f > 0.0f ? f : 0.0f;
        }
        else if (name == "clear_colour")
        {
            addonOverrides_.clearColour = static_cast<uint32_t>(value);
        }
        else if (name == "fps.divisor")
        {
            addonOverrides_.fpsDivisor = std::clamp(static_cast<int>(value), 0, 60);
        }
        else if (const int i = nameColourSlot(name); i >= 0)
        {
            ui::nameColourOverrides()[static_cast<std::size_t>(i)] = static_cast<uint32_t>(value);
        }
        else if (name == "window.party.visible")
        {
            shown.party = value != 0.0;
        }
        else if (name == "window.target.visible")
        {
            shown.target = value != 0.0;
        }
        else if (name == "window.alliance1.visible")
        {
            shown.alliance1 = value != 0.0;
        }
        else if (name == "window.alliance2.visible")
        {
            shown.alliance2 = value != 0.0;
        }
        else if (name == "window.macro_palette.visible")
        {
            shown.macroPalette = value != 0.0;
        }
        else if (name.starts_with("chat.filter.") && session && world.saveConfValid)
        {
            // C2S 0x0DB Kind 0 with the SAVE_CONF head's word changed (as the Config Chat Filters page sends it).
            auto              conf = world.saveConf;
            const std::size_t k    = static_cast<std::size_t>(name.back() - '0');
            const auto        v    = static_cast<uint32_t>(value);
            std::memcpy(conf.data() + 4 * k, &v, 4);
            std::vector<std::byte> p(0x24);
            std::memcpy(p.data() + 4, conf.data(), conf.size());
            session->send(0x0DB, std::move(p));
        }
        else if (name == "camera.distance.min" && f > 0.0f)
        {
            tune.nearDist = f;
        }
        else if (name == "camera.distance.max" && f > 0.0f)
        {
            tune.follow = f;
        }
        else if (name == "camera.battle_distance.min" && f > 0.0f)
        {
            tune.lockNear = f;
        }
        else if (name == "camera.battle_distance.max" && f > 0.0f)
        {
            tune.lockFar = f;
        }
        else if (name == "camera.pan.horizontal" && f > 0.0f)
        {
            tune.yawPerTick = f;
        }
        else if (name == "camera.pan.vertical" && f > 0.0f)
        {
            tune.risePerTick = f;
        }
        else if (name == "camera.battle_range" && f > 0.0f)
        {
            tune.lockCone = f;
        }
        else if (name == "map.all")
        {
            gameUi.allMaps.map = value != 0.0;
        }
        else if (name == "map.all.view")
        {
            gameUi.allMaps.view = value != 0.0;
        }
    };
    ashita::setNativeHooks(std::move(h));

    // The game's clock for its time functions (the `ffxi.time` library): the last zone-in's GameTime plus the seconds
    // since, as ntGameTimeGet keeps it; the date / time formats from string table 10.
    ashita::TimeSource time;
    time.gameTime = [this]() -> std::optional<uint32_t> {
        if (world.gameTimeAt == std::chrono::steady_clock::time_point{})
        {
            return std::nullopt;
        }
        const auto since = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - world.gameTimeAt).count();
        return world.gameTime + static_cast<uint32_t>(since);
    };
    time.text = [this](int id) { return formatText ? std::string(formatText->get(static_cast<std::size_t>(id))) : std::string(); };
    ashita::setTimeSource(std::move(time));

    // The game's target helpers (fancychat's <bt>, <r>, <scan> names).
    ashita::TargetHooks targets;
    targets.battleTarget = [this]() -> std::optional<std::pair<uint16_t, uint32_t>> {
        // SeekBattleActor (0x10077cb0): our party's ids (ourselves included), then the lowest-index actor with a model
        // whose claim (+0x188) is one of them and who is not dead (+0x170 not 2 / 3).
        std::vector<uint32_t> ids;
        for (const auto& m : world.party)
        {
            if (m.partyNo == 0 && ids.size() < 6)
            {
                ids.push_back(m.id);
            }
        }
        if (ids.empty() && world.charId != 0)
        {
            ids.push_back(world.charId);
        }
        const client::Entity* best = nullptr;
        for (const auto& e : world.entities)
        {
            if (e.claimId == 0 || e.animation == 2 || e.animation == 3 || e.hiddenByStatus(false) ||
                std::find(ids.begin(), ids.end(), e.claimId) == ids.end())
            {
                continue;
            }
            if (best == nullptr || e.index < best->index)
            {
                best = &e;
            }
        }
        return best ? std::optional(std::pair(best->index, best->id)) : std::nullopt;
    };
    targets.lastTeller = [this] { return world.lastTellFrom; };
    targets.zoneName   = [this](uint16_t actIndex) {
        // GetMonsterName (0x1008a810): the zone's name table record whose id's low 10 bits are the index.
        for (const auto& [id, name] : zoneNames)
        {
            if ((id & 0x3FF) == (actIndex & 0x3FFu))
            {
                return name;
            }
        }
        return std::string();
    };
    ashita::setTargetHooks(std::move(targets));

    // The macro palette (XIUI's macro bar, macrofix): index 0..9 the Ctrl row, 10..19 the Alt row; books / pages
    // 0-based on the game's side, 1-based here.
    ashita::MacroHooks m;
    m.canUse = [this] { return !gameUi.typing() && !(cutscene && cutscene->active()); };
    m.name   = [this](int i) { return gameUi.macroName(i / 10, i % 10 + 1); };
    m.line   = [this](int i, int l) { return gameUi.macroLine(i / 10, i % 10 + 1, l); };
    m.run    = [this](int row, int key) { gameUi.runMacro(row, key + 1); };
    m.set    = [this](int i, const std::string& title, const std::vector<std::string>& lines) { gameUi.setMacro(i / 10, i % 10 + 1, title, lines); };
    m.setBook = [this](int b) { return gameUi.setMacroBook(b + 1); };
    m.setPage = [this](int p) { return gameUi.setMacroSet(p + 1); };
    m.running = [this] { return gameUi.macroRunning(); };
    m.stop    = [this] { gameUi.stopMacros(); };
    ashita::setMacroHooks(std::move(m));
}

} // namespace xi::app

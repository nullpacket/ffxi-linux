// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/footsteps.h"
#include "audio/sound_system.h"
#include "client/action_effects.h"
#include "client_args.h"
#include "client/cutscene.h"
#include "client/doors.h"
#include "gfx/character_anim.h"
#include "client/search.h"
#include "client/session.h"
#include "dat/items.h"
#include "dat/spells.h"
#include "dat/sound.h"
#include "xi/bytes.h"
#include "dat/dat_index.h"
#include "dat/event.h"
#include "dat/texture.h"
#include "dat/zone.h"
#include "gfx/camera.h"
#include "gfx/context.h"
#include "gfx/window.h"
#include "gfx/effect_renderer.h"
#include "gfx/marker_renderer.h"
#include "dat/character.h"
#include "dat/motion.h"
#include "gfx/character_model.h"
#include "gfx/environment.h"
#include "gfx/skinned_renderer.h"
#include "gfx/sky_renderer.h"
#include "gfx/zone_scene.h"
#include <SDL3/SDL.h>
#include "dat/dll_data.h"
#include "dat/ui.h"
#include "client/emotes.h"
#include "gfx/scene_target.h"
#include "ui/game_ui.h"
#include "ui/keymap.h"
#include "xi/settings.h"
#include "ui/layout_view.h"
#include "ui/ui_draw.h"
#include <imgui.h>
#include <cstring>
#include <thread>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <spdlog/spdlog.h>
#include <fmt/ranges.h>
#include <algorithm>
#include <map>
#include <unordered_set>
#include <future>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

// xi-client's chat line: commands (/say ... /tell ... /search ... about 150 of them, docs/commands.md) and chat.
// Moved out of main.cpp's frame loop; ChatContext carries the loop state it reads and changes.
namespace xi::app
{

struct ChatContext
{
    std::unique_ptr<client::Session>& session;
    const client::WorldSnapshot& world;
    ui::GameUiActions& uiActions;
    std::future<std::vector<std::string>>& searchJob;
    const std::optional<dat::StringTable>& clientText;
    Args& args;
    const dat::ZoneMaps& zoneMaps;
    const std::optional<dat::StringTable>& usageText;
    audio::SoundSystem& sound;
    uint8_t& namesHidden;
    client::Entity& selfEntity;
    std::unique_ptr<gfx::ZoneScene>& scene;
    bool& quitting;
    const client::Entity*& outerTarget;
    bool& lockOn;
    const std::optional<dat::KeyItemTable>& keyItemNames;
    bool& hideTrusts;
    bool& ignoreTrusts;
    bool& ignorePet;
    uint32_t& followId;
    glm::vec3& feet;
    std::unordered_map<std::string, bool>& configFlags;
    int& blacklistAsked;
    std::shared_ptr<const dat::BattleText>& battleText;
    bool& autorun;
    const std::optional<dat::StringTable>& areaNames;
    std::optional<std::string>& bugReport; // /bugreport [text]: the request for ClientApp (goal 48)
};

// A line typed (or from a macro): `kind` = the chat channel when it is not a command.
void runChatLine(ChatContext& c, const std::string& line, uint8_t kind);

} // namespace xi::app

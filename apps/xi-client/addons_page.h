// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Config -> Add-ons (goal 47): the installed Ashita addons in the game's own list windows ("menu inventor"), with
// load / unload / reload, start at login (per character), the settings files Ashita's settings lib writes, each
// addon's errors, and which Ashita release the layer answers as.
#include "ashita/addon_info.h"
#include "ui/game_ui.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace xi::ashita
{
class AddonManager;
}

namespace xi::app
{

class AddonsPage
{
public:
    // What the page needs from the client; any may be empty.
    struct Host
    {
        std::function<ashita::AddonManager*()>        manager;   // null when addons are off
        std::function<std::filesystem::path()>        root;      // the Ashita install (client.ini ashita_dir)
        std::function<std::string()>                  character; // "" before the character is known
        std::function<uint32_t()>                     charId;
        std::function<std::string()>                  release;   // "auto", "latest" or "horizonxi"
        std::function<void(const std::string&)>       setRelease; // saves it and restarts the addons
        std::function<void(const std::string&)>       note;      // a line in the chat log
        std::function<void(const std::string&)>       command;   // run a chat line as typed (the addon's own command)
    };
    static constexpr int kTagList     = 0x4141;
    static constexpr int kTagAddon    = 0x4142;
    static constexpr int kTagSettings = 0x4143;

    explicit AddonsPage(Host host);

    // GameUi::takeAppChoice / takeAppAdjust answers; false when the tag is not the page's.
    bool onChoice(ui::GameUi& ui, int tag, int value);
    bool onAdjust(ui::GameUi& ui, int tag, int value, int dir);
    // A chat line: /addons opens the page, /addons set ADDON FILE KEY VALUE writes a setting. True when taken.
    bool command(ui::GameUi& ui, const std::string& line);
    // Once a frame: the character's startup list once it is known; the open pages follow the addons' state.
    void frame(ui::GameUi& ui);

    // The file a character's startup list lives in (~/.config/ffxi-linux/addons/<Character>.txt).
    static std::filesystem::path startupFile(const std::string& character);

private:
    Host                                host_;
    std::vector<ashita::AddonInfo>      addons_;
    int                                 current_ = -1;  // addon on the addon page
    std::vector<std::filesystem::path>  files_;         // its settings files
    std::size_t                         file_ = 0;      // the one on the settings page
    std::vector<ashita::SettingField>   fields_;
    std::string                         startedFor_;    // the character whose startup list ran
    std::vector<std::string>            lastLoaded_;    // to refresh the pages when it changes

    void openList(ui::GameUi& ui);
    void openAddon(ui::GameUi& ui, int i);
    void openSettings(ui::GameUi& ui, std::size_t file);
    void refresh(ui::GameUi& ui);
    std::vector<ui::GameUi::AppRow> listRows() const;
    std::vector<ui::GameUi::AppRow> addonRows() const;
    std::vector<ui::GameUi::AppRow> settingsRows() const;
    [[nodiscard]] bool              isLoaded(const std::string& name) const;
    [[nodiscard]] bool              startsAtLogin(const std::string& name) const;
    void                            setStartsAtLogin(const std::string& name, bool on);
    bool                            writeField(ui::GameUi& ui, std::size_t field, const std::string& value);
    void                            note(const std::string& s) const;
};

} // namespace xi::app

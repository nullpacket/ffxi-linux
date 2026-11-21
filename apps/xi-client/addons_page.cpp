// SPDX-License-Identifier: GPL-3.0-or-later
// Config -> Add-ons (goal 47): see addons_page.h. The pages are the game's list window ("menu inventor") fed by
// GameUi::openAppPage, so they take the window type, font, cursor and sounds of the other menus.
#include "addons_page.h"

#include "ashita/ashita.h"
#include "xi/ashita_dir.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fmt/format.h>
#include <sstream>

namespace xi::app
{
namespace
{

constexpr const char* kLayout = "menu inventor";
constexpr int         kReleaseRow = 1000;
constexpr int         kFileRow    = 1000;
constexpr int         kFolderRow  = 1001; // information rows (not choosable)
constexpr int         kLibsRow    = 1002;
constexpr int         kEmptyRow   = 1003;
constexpr std::size_t kLabelMax   = 17; // characters that fit beside the right column

std::string lowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string clip(const std::string& s, std::size_t n)
{
    return s.size() <= n ? s : s.substr(0, n - 2) + "..";
}

// A settings file's fields by key: addons rewrite their file in table order, which changes between saves, so the
// page keeps its rows (and the cursor) in one order.
std::vector<ashita::SettingField> readSorted(const std::filesystem::path& file)
{
    auto f = ashita::readSettings(file);
    std::stable_sort(f.begin(), f.end(), [](const ashita::SettingField& a, const ashita::SettingField& b) { return a.key() < b.key(); });
    return f;
}

std::string releaseName(const std::string& r)
{
    const auto l = lowerCopy(r);
    return l == "latest" ? "Latest" : l == "horizonxi" ? "HorizonXI" : "Auto";
}

} // namespace

AddonsPage::AddonsPage(Host host)
: host_(std::move(host))
{
}

std::filesystem::path AddonsPage::startupFile(const std::string& character)
{
    std::filesystem::path base;
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
    {
        base = x;
    }
    else if (const char* h = std::getenv("HOME"); h && *h)
    {
        base = std::filesystem::path(h) / ".config";
    }
    return base / "ffxi-linux" / "addons" / (character + ".txt");
}

void AddonsPage::note(const std::string& s) const
{
    if (host_.note)
    {
        host_.note(s);
    }
}

bool AddonsPage::isLoaded(const std::string& name) const
{
    const auto* m = host_.manager ? host_.manager() : nullptr;
    if (!m)
    {
        return false;
    }
    const auto l = m->loaded();
    return std::any_of(l.begin(), l.end(), [&](const std::string& n) { return lowerCopy(n) == lowerCopy(name); });
}

bool AddonsPage::startsAtLogin(const std::string& name) const
{
    const auto who = host_.character ? host_.character() : std::string();
    if (who.empty())
    {
        return false;
    }
    const auto l = ashita::readStartupList(startupFile(who));
    return std::any_of(l.begin(), l.end(), [&](const std::string& n) { return lowerCopy(n) == lowerCopy(name); });
}

void AddonsPage::setStartsAtLogin(const std::string& name, bool on)
{
    const auto who = host_.character ? host_.character() : std::string();
    if (who.empty())
    {
        note("[Addons] Log in first: the startup list is kept per character.");
        return;
    }
    const auto file = startupFile(who);
    auto       l    = ashita::readStartupList(file);
    std::erase_if(l, [&](const std::string& n) { return lowerCopy(n) == lowerCopy(name); });
    if (on)
    {
        l.push_back(name);
    }
    if (!ashita::writeStartupList(file, l))
    {
        note(fmt::format("[Addons] Could not save {}.", file.string()));
    }
}

std::vector<ui::GameUi::AppRow> AddonsPage::listRows() const
{
    std::vector<ui::GameUi::AppRow> rows;
    const auto*                     m = host_.manager ? host_.manager() : nullptr;
    {
        ui::GameUi::AppRow r;
        r.label      = "Ashita release";
        r.right      = releaseName(host_.release ? host_.release() : std::string());
        r.value      = kReleaseRow;
        r.adjustable = true;
        r.help       = m ? fmt::format("Add-ons see Ashita {}. Left / Right: change (restarts them).", ashita::profileVersion(m->profile()))
                         : std::string("Which Ashita the add-ons see. Left / Right: change.");
        rows.push_back(std::move(r));
    }
    const auto root = host_.root ? host_.root() : std::filesystem::path();
    auto info = [&](std::string label, std::string right, int value, std::string help) {
        ui::GameUi::AppRow r;
        r.label   = std::move(label);
        r.right   = std::move(right);
        r.value   = value;
        r.help    = std::move(help);
        r.enabled = false;
        rows.push_back(std::move(r));
    };
    if (!root.empty())
    {
        info("Folder", clip(root.filename().string() + "/addons", 12), kFolderRow,
             fmt::format("Add-ons are read from {}: one folder per add-on.", (root / "addons").string()));
        if (!xi::ashitadir::libsPresent(root))
        {
            info("! No libraries", "", kLibsRow, xi::ashitadir::libsMissingHint(root));
        }
    }
    if (addons_.empty())
    {
        info("(no add-ons)", "", kEmptyRow,
             root.empty() ? std::string("No Ashita folder.")
                          : fmt::format("Put Ashita add-on folders in {} (e.g. addons/fps/fps.lua), then open this page again.",
                                        (root / "addons").string()));
    }
    for (std::size_t i = 0; i < addons_.size(); ++i)
    {
        const auto&        a = addons_[i];
        ui::GameUi::AppRow r;
        r.label = clip(a.name, kLabelMax);
        const bool on    = isLoaded(a.name);
        const bool login = startsAtLogin(a.name);
        const bool err   = m && !m->errors(a.name).empty();
        r.right          = err ? "Error" : std::string(on ? "On" : "Off") + (login ? "*" : "");
        r.value          = static_cast<int>(i);
        std::string desc = a.description.empty() ? std::string("(no description)") : a.description;
        if (!a.version.empty())
        {
            desc += fmt::format(" v{}", a.version);
        }
        if (!a.author.empty())
        {
            desc += fmt::format(" by {}", a.author);
        }
        if (!a.needs.empty())
        {
            desc += fmt::format(" (needs {})", a.needs);
        }
        if (login)
        {
            desc += " * starts at login.";
        }
        r.help = desc;
        rows.push_back(std::move(r));
    }
    return rows;
}

std::vector<ui::GameUi::AppRow> AddonsPage::addonRows() const
{
    std::vector<ui::GameUi::AppRow> rows;
    if (current_ < 0 || static_cast<std::size_t>(current_) >= addons_.size())
    {
        return rows;
    }
    const auto& a     = addons_[static_cast<std::size_t>(current_)];
    const bool  on    = isLoaded(a.name);
    const bool  login = startsAtLogin(a.name);
    const auto* m     = host_.manager ? host_.manager() : nullptr;
    auto add = [&](std::string label, std::string right, int value, std::string help, bool enabled = true, bool adjustable = false) {
        ui::GameUi::AppRow r;
        r.label      = std::move(label);
        r.right      = std::move(right);
        r.value      = value;
        r.help       = std::move(help);
        r.enabled    = enabled;
        r.adjustable = adjustable;
        rows.push_back(std::move(r));
    };
    add(on ? "Unload" : "Load", "", 1, on ? "Unload this add-on now." : "Load this add-on now.", m != nullptr);
    add("Reload", "", 2, "Unload and load it again.", m != nullptr && on);
    add("Start at login", login ? "On" : "Off", 3, "Load it whenever this character logs in.", true, true);
    add("Settings", files_.empty() ? "" : fmt::format("{}", files_.size()), 4,
        files_.empty() ? "No settings file yet: load the add-on once and it writes one." : "Edit its saved settings.", !files_.empty());
    add(clip("Run /" + lowerCopy(a.name), kLabelMax), "", 5, "Run the add-on's own command (most open their settings or help).", on);
    add("Information", "", 6,
        fmt::format("{}{}{}{}", a.description.empty() ? a.name : a.description, a.version.empty() ? "" : " v" + a.version,
                    a.author.empty() ? "" : " by " + a.author, a.link.empty() ? "" : " " + a.link));
    if (m)
    {
        const auto errs = m->errors(a.name);
        for (std::size_t i = 0; i < errs.size(); ++i)
        {
            add(clip("! " + errs[i], kLabelMax + 6), "", 100 + static_cast<int>(i), errs[i], false);
        }
    }
    return rows;
}

std::vector<ui::GameUi::AppRow> AddonsPage::settingsRows() const
{
    std::vector<ui::GameUi::AppRow> rows;
    if (file_ < files_.size())
    {
        ui::GameUi::AppRow r;
        r.label      = "File";
        r.right      = clip(files_[file_].stem().string(), 12);
        r.value      = kFileRow;
        r.adjustable = files_.size() > 1;
        r.enabled    = files_.size() > 1;
        r.help       = files_[file_].string();
        rows.push_back(std::move(r));
    }
    for (std::size_t i = 0; i < fields_.size(); ++i)
    {
        const auto&        f = fields_[i];
        ui::GameUi::AppRow r;
        r.label = clip(f.key(), kLabelMax);
        r.right = clip(f.display(), 12);
        r.value = static_cast<int>(i);
        using T = ashita::SettingField::Type;
        const bool colour = f.display().rfind("0x", 0) == 0;
        r.enabled    = f.type != T::Other;
        r.adjustable = f.type == T::Bool || (f.type == T::Number && !colour);
        r.help       = f.type == T::Bool     ? fmt::format("{} = {}: confirm or Left / Right to switch.", f.key(), f.value)
                     : f.type == T::Number   ? fmt::format("{} = {}: Left / Right step it, confirm to type a value.", f.key(), f.display())
                     : f.type == T::String   ? fmt::format("{} = {}: confirm to type a new value.", f.key(), f.display())
                                             : fmt::format("{} = {} (not editable here).", f.key(), f.value);
        rows.push_back(std::move(r));
    }
    return rows;
}

void AddonsPage::openList(ui::GameUi& ui)
{
    const auto root = host_.root ? host_.root() : std::filesystem::path();
    addons_         = root.empty() ? std::vector<ashita::AddonInfo>{} : ashita::scanAddons(root);
    if (root.empty())
    {
        note("[Addons] Add-ons are off (client.ini ashita_dir = off, or no game folder).");
    }
    else if (!xi::ashitadir::libsPresent(root))
    {
        note("[Addons] " + xi::ashitadir::libsMissingHint(root));
    }
    ui.openAppPage(kTagList, kLayout, "Add-ons", listRows(), "Select an add-on.");
    lastLoaded_ = host_.manager && host_.manager() ? host_.manager()->loaded() : std::vector<std::string>{};
}

void AddonsPage::openAddon(ui::GameUi& ui, int i)
{
    current_ = i;
    const auto& a  = addons_[static_cast<std::size_t>(i)];
    const auto  rt = host_.root ? host_.root() : std::filesystem::path();
    files_         = ashita::settingsFiles(rt, a.name, host_.character ? host_.character() : std::string(), host_.charId ? host_.charId() : 0);
    ui.openAppPage(kTagAddon, kLayout, clip(a.name, 14), addonRows());
}

void AddonsPage::openSettings(ui::GameUi& ui, std::size_t file)
{
    file_   = file;
    fields_ = file_ < files_.size() ? readSorted(files_[file_]) : std::vector<ashita::SettingField>{};
    if (ui.appPageOpen(kTagSettings))
    {
        ui.updateAppPage(kTagSettings, settingsRows());
        return;
    }
    ui.openAppPage(kTagSettings, kLayout, "Settings", settingsRows());
}

void AddonsPage::refresh(ui::GameUi& ui)
{
    if (ui.appPageOpen(kTagList))
    {
        ui.updateAppPage(kTagList, listRows());
    }
    if (ui.appPageOpen(kTagAddon))
    {
        ui.updateAppPage(kTagAddon, addonRows());
    }
    if (ui.appPageOpen(kTagSettings))
    {
        ui.updateAppPage(kTagSettings, settingsRows());
    }
}

bool AddonsPage::writeField(ui::GameUi& ui, std::size_t field, const std::string& value)
{
    if (field >= fields_.size() || file_ >= files_.size() || current_ < 0)
    {
        return false;
    }
    const auto name = addons_[static_cast<std::size_t>(current_)].name;
    const auto key  = fields_[field].key(); // by key: the addon may write its file in another order
    auto*      m    = host_.manager ? host_.manager() : nullptr;
    // An addon saves its settings when it unloads, so a loaded one is unloaded first and loaded again after.
    const bool reload = m && isLoaded(name);
    if (reload)
    {
        m->unload(name);
        fields_ = readSorted(files_[file_]); // what it saved
    }
    const auto it  = std::find_if(fields_.begin(), fields_.end(), [&](const ashita::SettingField& f) { return f.key() == key; });
    bool       ok  = it != fields_.end() && ashita::writeSetting(files_[file_], *it, value);
    if (reload)
    {
        m->load(name);
    }
    fields_ = readSorted(files_[file_]);
    if (!ok)
    {
        note(fmt::format("[Addons] Could not write {} in {}.", key, files_[file_].string()));
    }
    refresh(ui);
    return ok;
}

bool AddonsPage::onChoice(ui::GameUi& ui, int tag, int value)
{
    if (tag == ui::GameUi::kAddonsTag)
    {
        openList(ui);
        return true;
    }
    if (tag != kTagList && tag != kTagAddon && tag != kTagSettings)
    {
        return false;
    }
    if (value < 0)
    {
        refresh(ui); // went back: the page under it may show changed state
        return true;
    }
    auto* m = host_.manager ? host_.manager() : nullptr;
    if (tag == kTagList)
    {
        if (value == kReleaseRow)
        {
            onAdjust(ui, tag, value, 1);
        }
        else if (value >= 0 && static_cast<std::size_t>(value) < addons_.size())
        {
            openAddon(ui, value);
        }
        return true;
    }
    if (tag == kTagAddon && current_ >= 0)
    {
        const auto& a = addons_[static_cast<std::size_t>(current_)];
        switch (value)
        {
            case 1:
                if (m)
                {
                    if (isLoaded(a.name))
                    {
                        m->unload(a.name);
                    }
                    else
                    {
                        m->load(a.name);
                    }
                }
                break;
            case 2:
                if (m && isLoaded(a.name))
                {
                    m->unload(a.name);
                    m->load(a.name);
                }
                break;
            case 3: setStartsAtLogin(a.name, !startsAtLogin(a.name)); break;
            case 4: openSettings(ui, 0); return true;
            case 5:
                if (host_.command)
                {
                    host_.command("/" + lowerCopy(a.name));
                }
                break;
            case 6:
                note(fmt::format("[Addons] {}{}{}{}", a.name, a.version.empty() ? "" : " v" + a.version, a.author.empty() ? "" : " by " + a.author,
                                 a.description.empty() ? "" : ": " + a.description));
                if (!a.link.empty())
                {
                    note(fmt::format("[Addons] {}", a.link));
                }
                break;
            default: break;
        }
        const auto rt = host_.root ? host_.root() : std::filesystem::path();
        files_        = ashita::settingsFiles(rt, a.name, host_.character ? host_.character() : std::string(), host_.charId ? host_.charId() : 0);
        refresh(ui);
        return true;
    }
    if (tag == kTagSettings)
    {
        if (value == kFileRow)
        {
            onAdjust(ui, tag, value, 1);
            return true;
        }
        const auto i = static_cast<std::size_t>(value);
        if (i >= fields_.size())
        {
            return true;
        }
        const auto& f = fields_[i];
        using T       = ashita::SettingField::Type;
        if (f.type == T::Bool)
        {
            writeField(ui, i, f.value == "true" ? "false" : "true");
        }
        else if (f.type == T::Number || f.type == T::String)
        {
            // The value is typed on the chat line, as the game's own text entries are.
            ui.startTyping(fmt::format("/addons set {} {} {} ", addons_[static_cast<std::size_t>(current_)].name, file_ + 1, f.key()));
        }
        return true;
    }
    return true;
}

bool AddonsPage::onAdjust(ui::GameUi& ui, int tag, int value, int dir)
{
    if (tag == kTagList && value == kReleaseRow)
    {
        static const char* kOrder[] = { "auto", "latest", "horizonxi" };
        const auto cur = lowerCopy(host_.release ? host_.release() : std::string("auto"));
        int        i   = 0;
        for (int k = 0; k < 3; ++k)
        {
            if (cur == kOrder[k])
            {
                i = k;
            }
        }
        i = (i + (dir > 0 ? 1 : 2)) % 3;
        if (host_.setRelease)
        {
            host_.setRelease(kOrder[i]);
        }
        refresh(ui);
        return true;
    }
    if (tag == kTagAddon && value == 3 && current_ >= 0)
    {
        const auto& a = addons_[static_cast<std::size_t>(current_)];
        setStartsAtLogin(a.name, !startsAtLogin(a.name));
        refresh(ui);
        return true;
    }
    if (tag == kTagSettings)
    {
        if (value == kFileRow)
        {
            if (!files_.empty())
            {
                openSettings(ui, (file_ + files_.size() + static_cast<std::size_t>(dir > 0 ? 1 : files_.size() - 1)) % files_.size());
            }
            return true;
        }
        const auto i = static_cast<std::size_t>(value);
        if (i < fields_.size())
        {
            const auto& f = fields_[i];
            if (f.type == ashita::SettingField::Type::Bool)
            {
                writeField(ui, i, f.value == "true" ? "false" : "true");
            }
            else if (f.type == ashita::SettingField::Type::Number)
            {
                // Whole numbers step by 1, decimals by 0.1.
                const bool   frac = f.value.find('.') != std::string::npos;
                const double v    = std::strtod(f.value.c_str(), nullptr) + (frac ? 0.1 : 1.0) * dir;
                writeField(ui, i, frac ? fmt::format("{:.2f}", v) : fmt::format("{}", static_cast<long long>(v)));
            }
        }
        return true;
    }
    return tag == kTagList || tag == kTagAddon;
}

bool AddonsPage::command(ui::GameUi& ui, const std::string& line)
{
    std::istringstream in(line);
    std::string        cmd, sub;
    in >> cmd >> sub;
    if (lowerCopy(cmd) != "/addons")
    {
        return false;
    }
    if (lowerCopy(sub) != "set")
    {
        if (!ui.appPageOpen(kTagList))
        {
            openList(ui);
        }
        return true;
    }
    // /addons set ADDON FILE KEY VALUE...
    std::string addon, key;
    std::size_t file = 0;
    in >> addon >> file >> key;
    std::string value;
    std::getline(in, value);
    value.erase(0, value.find_first_not_of(' '));
    if (addons_.empty())
    {
        const auto root = host_.root ? host_.root() : std::filesystem::path();
        addons_         = root.empty() ? std::vector<ashita::AddonInfo>{} : ashita::scanAddons(root);
    }
    const auto it = std::find_if(addons_.begin(), addons_.end(), [&](const ashita::AddonInfo& a) { return lowerCopy(a.name) == lowerCopy(addon); });
    if (it == addons_.end() || file == 0 || value.empty())
    {
        note("[Addons] Usage: /addons set ADDON FILE KEY VALUE (Config -> Add-ons fills it in).");
        return true;
    }
    if (current_ != static_cast<int>(it - addons_.begin()))
    {
        current_      = static_cast<int>(it - addons_.begin());
        const auto rt = host_.root ? host_.root() : std::filesystem::path();
        files_        = ashita::settingsFiles(rt, it->name, host_.character ? host_.character() : std::string(), host_.charId ? host_.charId() : 0);
    }
    if (file > files_.size())
    {
        note("[Addons] No such settings file.");
        return true;
    }
    file_   = file - 1;
    fields_ = readSorted(files_[file_]);
    const auto f = std::find_if(fields_.begin(), fields_.end(), [&](const ashita::SettingField& x) { return x.key() == key; });
    if (f == fields_.end())
    {
        note(fmt::format("[Addons] {} has no setting {}.", it->name, key));
        return true;
    }
    if (f->type == ashita::SettingField::Type::Number)
    {
        // Colours may be typed as 0xAARRGGBB.
        char*            end = nullptr;
        const long long  v   = std::strtoll(value.c_str(), &end, 0);
        const double     d   = std::strtod(value.c_str(), nullptr);
        if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0)
        {
            value = fmt::format("{}", static_cast<unsigned long long>(v));
        }
        else if (value.find('.') == std::string::npos && end && *end == 0)
        {
            value = fmt::format("{}", v);
        }
        else
        {
            value = fmt::format("{}", d);
        }
    }
    if (writeField(ui, static_cast<std::size_t>(f - fields_.begin()), value))
    {
        note(fmt::format("[Addons] {}: {} = {}", it->name, key, value));
    }
    // Typing closed the menus: back to the settings page it came from (list -> add-on -> settings, so cancel walks
    // back through them).
    if (!ui.appPageOpen(kTagSettings))
    {
        const int         back = current_;
        const std::size_t f2   = file_;
        if (!ui.appPageOpen(kTagList))
        {
            openList(ui);
        }
        if (back >= 0 && static_cast<std::size_t>(back) < addons_.size())
        {
            openAddon(ui, back);
            openSettings(ui, f2);
        }
    }
    return true;
}

void AddonsPage::frame(ui::GameUi& ui)
{
    auto*      m   = host_.manager ? host_.manager() : nullptr;
    const auto who = host_.character ? host_.character() : std::string();
    if (m && !who.empty() && startedFor_ != who)
    {
        startedFor_ = who;
        for (const auto& n : ashita::readStartupList(startupFile(who)))
        {
            if (!isLoaded(n))
            {
                m->load(n);
            }
        }
    }
    const auto now = m ? m->loaded() : std::vector<std::string>{};
    if (now != lastLoaded_)
    {
        lastLoaded_ = now;
        refresh(ui);
    }
}

} // namespace xi::app

// SPDX-License-Identifier: GPL-3.0-or-later
// The memory stand-ins (src/ashita/scan_registry.h, prelude_scan.lua; docs/ashita-compat.md "Native stand-ins for
// memory signatures"): test addons scan for the signatures real addons use and read the results the way those addons
// do, through ashita.memory and through ffi. The emulated signatures answer with or without the game image; the image
// test needs FFXI_ROOT.
#include "xi/install.h"
#include "ashita/ashita.h"
#include "ashita/scan_registry.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using xi::ashita::AddonManager;
using xi::ashita::Frame;
using xi::ashita::Profile;

namespace
{

struct Harness
{
    fs::path                                   dir;
    std::vector<std::string>                   chat;
    std::shared_ptr<xi::client::WorldSnapshot> world = std::make_shared<xi::client::WorldSnapshot>();
    std::unique_ptr<AddonManager>              m;

    Harness()
    {
        dir = fs::temp_directory_path() / ("xi_ashita_scan_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir / "addons");
        fs::create_directories(dir / "config" / "ashita");
        world->inZone   = true;
        world->charName = "Tester";
        xi::ashita::Host h;
        h.chat = [this](const std::string& text, int) { chat.push_back(text); };
        m      = std::make_unique<AddonManager>(dir, Profile::HorizonXI, xi::ashita::Resources{}, std::move(h));
    }
    ~Harness()
    {
        m.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Harness(const Harness&)            = delete;
    Harness& operator=(const Harness&) = delete;

    void addon(const std::string& name, const std::string& code) const
    {
        fs::create_directories(dir / "addons" / name);
        std::ofstream(dir / "addons" / name / (name + ".lua")) << code;
    }
    void frame()
    {
        Frame f;
        f.world   = std::make_shared<xi::client::WorldSnapshot>(*world);
        f.windowW = 1280;
        f.windowH = 720;
        f.menuW   = 800;
        f.menuH   = 600;
        m->update(std::move(f));
    }
    void run(const std::string& line)
    {
        m->command(line, 1);
        frame();
    }
    [[nodiscard]] std::vector<std::string> failures() const
    {
        std::vector<std::string> v;
        for (const auto& l : chat)
        {
            if (l.rfind("fail ", 0) == 0 || l.find(".lua:") != std::string::npos || l.find("traceback") != std::string::npos)
            {
                v.push_back(l);
            }
        }
        return v;
    }
    [[nodiscard]] std::string log() const
    {
        std::string s;
        for (const auto& l : chat)
        {
            s += l + "\n";
        }
        return s;
    }
    [[nodiscard]] int passed() const
    {
        return static_cast<int>(std::count_if(chat.begin(), chat.end(), [](const std::string& l) { return l.rfind("ok ", 0) == 0; }));
    }
};

const std::string kCheck = R"lua(
local ffi = require('ffi');
local function check(name, ok, why)
    if (ok) then print('ok ' .. name); else print('fail ' .. name .. ': ' .. tostring(why)); end
end
local mem = ashita.memory;
)lua";

// What the HorizonXI libs/lin/ffxi.lua, LuAshitacast, statustimers, ttimers, scaling.lua and config read.
const std::string kReaders = kCheck + R"lua(
local menuBase = mem.find('FFXiMain.dll', 0, '8B480C85C974??8B510885D274??3B05', 16, 0);
local function menu_name()
    local g = mem.read_uint32(menuBase);
    local o = mem.read_uint32(g);
    if (o == 0) then return ''; end
    return (mem.read_string(mem.read_uint32(o + 4) + 0x46, 16):gsub('%z', ''));
end
local ev = mem.find('FFXiMain.dll', 0, 'A0????????84C0741AA1????????85C0741166A1????????663B05????????0F94C0C3', 0, 0);
local hid = mem.find('FFXiMain.dll', 0, '8B4424046A016A0050B9????????E8????????F6D81BC040C3', 0, 0);
local chat = mem.find('FFXiMain.dll', 0, '83EC??B9????????E8????????0FBF4C24??84C0', 0x04, 0);
local rt = mem.find('FFXiMain.dll', 0, '8B0D????????8B410C8B49108D04808D04808D04808D04C1C3', 2, 0);
local weather = mem.find('FFXiMain.dll', 0, '66A1????????663D????72', 0, 0);
local zone = mem.find('FFXiMain.dll', 0, '8B8C24040100008B90????????0BD18990????????8B15????????8B82', 0, 0);
local vana = mem.find('FFXiMain.dll', 0, 'B0015EC390518B4C24088D4424005068', 0, 0);
local scaling = mem.find('FFXiMain.dll', 0, 'A1????????85C05E74????80', 0, 0);

local function state()
    local zp = mem.read_uint32(mem.read_uint32(zone, 0x17));
    local s = {
        menu = menu_name(),
        event = mem.read_uint8(mem.read_uint32(ev + 1)),
        hidden = mem.read_uint8(mem.read_uint32(hid + 10) + 0xB4),
        chat = mem.read_uint8(mem.read_uint32(chat) + 0xF1),
        utc = mem.read_uint32(mem.read_uint32(mem.read_uint32(rt)) + 0x0C),
        weather = mem.read_uint8(mem.read_uint32(weather + 2)),
        mog = bit.band(mem.read_uint32(zp + mem.read_uint32(zone, 9)), 0x100),
        vana = mem.read_uint32(mem.read_uint32(vana + 0x34) + 0x0C) + 92514960,
    };
    local sp = mem.read_uint32(mem.read_uint32(scaling + 1));
    s.w, s.h, s.mw, s.mh = mem.read_uint16(sp + 0x10), mem.read_uint16(sp + 0x12), mem.read_uint16(sp + 0x14), mem.read_uint16(sp + 0x16);
    -- The same through ffi, as tCrossBar / blusets style code does.
    s.ffi_hidden = ffi.cast('uint8_t*', ffi.cast('uint32_t*', hid + 10)[0])[0xB4];
    return s;
end

check('found', menuBase ~= 0 and ev ~= 0 and hid ~= 0 and chat ~= 0 and rt ~= 0 and weather ~= 0 and zone ~= 0 and vana ~= 0 and scaling ~= 0);
check('unknown pattern answers 0', mem.find('FFXiMain.dll', 0, 'DEADBEEF????CAFE', 0, 0) == 0);
check('other module answers 0', mem.find('polcore.dll', 0, '8B0D????????', 0, 0) == 0);
check('reads outside answer 0', mem.read_uint32(0) == 0 and mem.read_uint32(16) == 0 and mem.read_string(8, 4) == '' and mem.read_uint8(2 ^ 40) == 0);
check('writes outside do nothing', mem.write_uint32(16, 5) == false);
check('find is stable', mem.find('FFXiMain.dll', 0, '8B480C85C974??8B510885D274??3B05', 16, 0) == menuBase);

ashita.events.register('command', 'cmd', function (e)
    local s = state();
    if (e.command == '/s1') then
        check('menu none', s.menu == '', s.menu);
        check('event off', s.event == 0, s.event);
        check('hidden off', s.hidden == 0 and s.ffi_hidden == 0, s.hidden);
        check('chat collapsed', s.chat == 0, s.chat);
        check('utc', math.abs(s.utc - os.time()) <= 2, s.utc);
        check('weather', s.weather == 6, s.weather);
        check('not in mog house', s.mog == 0, s.mog);
        check('vana clock', s.vana > 0 and math.abs(s.vana / 2.4 - (math.floor((os.time() - 1009810800) * 25 / 60) + 886 * 518400)) < 2, s.vana);
        check('scaling', s.w == 1280 and s.h == 720 and s.mw == 800 and s.mh == 600, ('%d %d %d %d'):format(s.w, s.h, s.mw, s.mh));
        e.blocked = true;
    elseif (e.command == '/s2') then
        check('menu map', s.menu == 'menu    map0', s.menu);
        check('menu is map', s.menu:match('menu%s+map.*') ~= nil);
        check('event on', s.event == 1, s.event);
        check('hidden on', s.hidden == 1 and s.ffi_hidden == 1, s.hidden);
        check('chat expanded', s.chat == 1, s.chat);
        check('in mog house', s.mog == 0x100, s.mog);
        e.blocked = true;
    end
end);
)lua";

const std::string kConfig = kCheck + R"lua(
ffi.cdef[[
    typedef int32_t (__cdecl* xt_get_config_value_t)(int32_t);
    typedef int32_t (__cdecl* xt_set_config_value_t)(int32_t, int32_t);
    typedef int32_t (__fastcall* xt_get_config_entry_t)(int32_t, int32_t, int32_t);
    typedef int32_t (__cdecl* xt_set_auto_offline_t)(int32_t);
    typedef struct { uintptr_t VTable; uint32_t key; int32_t value; uint32_t type; char pol[8]; int32_t min; int32_t max; int32_t def; uintptr_t cb; int32_t proc; } xt_FsConfigSubject;
]];
-- As config.lua does it.
local ptr = mem.find(0, 0, '8B0D????????85C974??8B44240450E8????????C383C8FFC3', 0, 0);
local get = ffi.cast('xt_get_config_value_t', ptr);
local set = ffi.cast('xt_set_config_value_t', mem.find(0, 0, '85C974??8B4424088B5424045052E8????????C383C8FFC3', -6, 0));
local info = ffi.cast('xt_get_config_entry_t', mem.find(0, 0, '8B490485C974108B4424048D14808D04508D0481C2040033C0C20400', 0, 0));
local offline = ffi.cast('xt_set_auto_offline_t', mem.find(0, 0, '568B74240885F674??B889888888F7EE', 0, 0));
local this = ffi.cast('uint32_t**', ptr + 2)[0][0];
check('config found', ptr ~= 0 and this ~= 0, this);
check('config get', get(42) == 7, get(42));
check('config get unknown', get(999) == -1);
check('config set', set(42, 9) == 0 and get(42) == 9);
check('config set refused', set(999, 1) == -1);
local cfg = ffi.cast('xt_FsConfigSubject*', info(this, 0, 42));
check('config info', cfg ~= nil and cfg.key == 42 and cfg.value == 9 and cfg.min == 0 and cfg.max == 10 and cfg.def == 3, cfg ~= nil and cfg.value);
check('config info unknown', info(this, 0, 999) == 0);
offline(1);
-- ime: two uint32_t** reads of a code operand (8 bytes on x64), then byte writes into the objects.
local input = ffi.cast('uint8_t*', (ffi.cast('uint32_t**', mem.find(0, 0, '8B0D????????81EC0401000053568B', 2, 0))[0])[0]);
input[0xF0EC] = 1; input[0xF10C] = 1;
local lang = ffi.cast('uint8_t*', (ffi.cast('uint32_t**', mem.find(0, 0, '8B0D????????83EC0C8A51188B410884D256', 2, 0))[0])[0]);
check('ime language', lang[8] == 1, lang[8]);
local patch = ffi.cast('uint8_t*', mem.find(0, 0, '83EC08A1????????5333DB56', 0x2F, 0));
patch[0] = 0xEB;
check('ime patch', patch[0] == 0xEB);
-- IPointerManager: an addon's own (timers' REALUTCSTAMP).
local pm = AshitaCore:GetPointerManager();
pm:Add('test:utc', 'FFXiMain.dll', '8B0D????????8B410C8B49108D04808D04808D04808D04C1C3', 2, 0);
local p = pm:Get('test:utc');
check('pointer manager', p ~= 0 and math.abs(mem.read_uint32(mem.read_uint32(mem.read_uint32(p)) + 0x0C) - os.time()) <= 2, p);
check('pointer manager unknown', pm:Get('no.such.pointer') == 0);
)lua";

// thotbar / tCrossBar's BLU set: the 'inventory' named pointer, then the offset read out of an instruction.
const std::string kBlu = kCheck + R"lua(
local bluOffset = mem.read_uint32(mem.find('FFXiMain.dll', 0, 'C1E1032BC8B0018D????????????B9????????F3A55F5E5B', 10, 0));
ashita.events.register('command', 'cmd', function (e)
    if (e.command ~= '/blu') then return; end
    e.blocked = true;
    local ptr = mem.read_uint32(AshitaCore:GetPointerManager():Get('inventory'));
    ptr = mem.read_uint32(ptr);
    check('inventory object', ptr ~= 0, ptr);
    local main = mem.read_array(ptr + bluOffset + 0x04, 0x14);
    local sub = mem.read_array(ptr + bluOffset + 0xA0, 0x14);
    check('blu main', main[1] == 1 and main[2] == 0x29 and main[20] == 0x7F, table.concat(main, ','));
    check('blu sub', sub[1] == 0x55 and sub[2] == 0, table.concat(sub, ','));
    check('blu job byte', mem.read_uint8(ptr + bluOffset) == 16);
end);
)lua";

// The patch addons, each the way it touches memory.
const std::string kPatches = kCheck + R"lua(
-- drawdistance
local dd = mem.find(0, 0, '8BC1487408D80D', 0, 0);
check('drawdistance found', dd ~= 0);
check('draw distance reads the client', math.abs(mem.read_float(mem.read_uint32(dd + 0x07)) - 1.5) < 1e-6, mem.read_float(mem.read_uint32(dd + 0x07)));
mem.write_float(mem.read_uint32(dd + 0x07), 3);
mem.write_float(mem.read_uint32(dd + 0x0F), 2);
-- hideparty
local p1 = mem.find(0, 0, '66C78182000000????C7818C000000????????C781900000', 0, 0);
local p2 = mem.find(0, 0, 'A1????????8B0D????????89442424A1????????33DB89', 0, 0);
check('hideparty found', p1 ~= 0 and p2 ~= 0);
local function vis(p, v)
    local ptr = mem.read_uint32(p);
    ptr = mem.read_uint32(ptr + 0x08);
    if (v ~= nil) then mem.write_uint8(ptr + 0x69, v); mem.write_uint8(ptr + 0x6A, v); end
    return mem.read_uint8(ptr + 0x69);
end
check('party shown', vis(mem.read_uint32(p1 + 0x19)) == 1);
vis(mem.read_uint32(p1 + 0x19), 0);
-- instantchat
local ic = mem.find(0, 0, '8BF174??8B46340FBF4E5485C07D??894E34', 0, 0);
check('instantchat found', ic ~= 0);
mem.write_array(ic + 0x1F, { 0x90, 0x90, 0x90 });
-- namecolors
local nc = mem.find(0, 0, 'B917000000B880808080BF', 0x0B, 0);
local tbl = mem.read_uint32(nc);
check('name colour default', mem.read_uint32(tbl + 4 * 3) == 0x80808080, mem.read_uint32(tbl + 4 * 3));
mem.write_uint32(tbl + 4 * 3, 0xC0FF0000);
-- filters, through ffi as it does
local fp = ffi.cast('uint32_t**', mem.find(0, 0, 'C3C74004000000008B0D????????81C1', 0x0A, 0));
local foff = ffi.cast('uint32_t*', ffi.cast('uint8_t*', fp) + 0x06);
local data = ffi.cast('uint32_t*', fp[0][0] + foff[0]);
data[1] = 0x1234;
-- xicamera
local maxs = mem.find('FFXiMain.dll', 0, 'D9442410D825????????51D80D', 0, 0);
check('camera max reads 6', math.abs(mem.read_float(mem.read_uint32(maxs + 0x06)) - 6) < 1e-6);
mem.write_float(mem.read_uint32(maxs + 0x06), 10);
-- unsupported: the clear colour
check('clearcolor fails', mem.find(0, 0, 'A1????????0508030000C3', 0, 0) == 0);
ashita.events.register('command', 'cmd', function (e)
    if (e.command ~= '/back') then return; end
    e.blocked = true;
    check('client change reaches memory', mem.read_float(mem.read_uint32(dd + 0x07)) == 5);
    check('client shows party again', vis(mem.read_uint32(p1 + 0x19)) == 1);
end);
)lua";

// XIUI's libs/ffxi/macros.lua, cut down: the same signatures, casts and calls.
const std::string kMacros = kCheck + R"lua(
ffi.cdef[[
    typedef bool        (__thiscall*    xt_canUse_f)(uint32_t);
    typedef bool        (__thiscall*    xt_clear_f)(uint32_t, uint32_t);
    typedef const char* (__thiscall*    xt_getName_f)(uint32_t, uint32_t);
    typedef const char* (__thiscall*    xt_getLine_f)(uint32_t, uint32_t, uint32_t);
    typedef void        (__thiscall*    xt_run_f)(uint32_t, uint8_t, uint8_t);
    typedef int32_t     (__thiscall*    xt_setBook_f)(uint32_t, uint32_t);
    typedef int32_t     (__thiscall*    xt_setPage_f)(uint32_t, uint32_t);
    typedef void        (__cdecl*       xt_setMacro_f)(uint32_t, const char*, const char*);
]];
local p = {
    macro      = mem.find('FFXiMain.dll', 0, '8B0D????????E8????????8B4424105EC38B15????????68', 2, 0),
    controller = mem.find('FFXiMain.dll', 0, 'A3????????EB??891D????????6A50E8', 1, 0),
    can_use    = mem.find('FFXiMain.dll', 0, '66A1????????5633F66685C074??0FBFC08B3485????????8B0D', 0, 0),
    clear      = mem.find('FFXiMain.dll', 0, '8B44240485C07C??83F8147D??8D1440C1E2052BD08D4C9104E8????????B001', 0, 0),
    get_name   = mem.find('FFXiMain.dll', 0, '8B44240485C07C??83F8147D??8D1440C1E2052BD08D4C9104E8????????85C0', 0, 0),
    get_line   = mem.find('FFXiMain.dll', 0, '8B44240485C07C??83F8147D??8B542408528D1440C1E2052BD08D4C9104E8????????C2080033C0C20800', 0, 0),
    run        = mem.find('FFXiMain.dll', 0, '8A44240453563C018BF175??8A5C24108B4E040FBEC350E8', 0, 0),
    set_book   = mem.find('FFXiMain.dll', 0, '8B442404568BF18B8EC01D00003BC875??8BC15EC20400', 0, 0),
    set_page   = mem.find('FFXiMain.dll', 0, '568BF1578B7C240C8B86C01D00008D04', 0, 0),
    set        = mem.find('FFXiMain.dll', 0, '8B442408538B5C24085657508BCBE8????????85C07C??8B', 0, 0),
    stop       = mem.find('FFXiMain.dll', 0, 'C781????????FFFFFFFFC3', 0, 0),
};
local all = true;
for k, v in pairs(p) do if (v == 0) then all = false; print('fail missing ' .. k); end end
check('macro pointers', all);
local obj = mem.read_uint32(mem.read_uint32(p.macro));
local ctl = mem.read_uint32(mem.read_uint32(p.controller));
check('objects', obj ~= 0 and ctl ~= 0);
check('can use', ffi.cast('xt_canUse_f', p.can_use)(ctl) == true);
check('name', ffi.string(ffi.cast('xt_getName_f', p.get_name)(obj, 11)) == 'Haste');
check('line', ffi.string(ffi.cast('xt_getLine_f', p.get_line)(obj, 11, 1)) == '/ma "Haste" <t>');
ffi.cast('xt_run_f', p.run)(ctl, 2, 1);
check('set book', ffi.cast('xt_setBook_f', p.set_book)(obj, 3) == 1);
check('set page', ffi.cast('xt_setPage_f', p.set_page)(obj, 2 * (4 + 1) - 2) == 1);
local str = '/ja "Provoke" <t>' .. ('\0'):rep(61 - 17) .. '/wait 1' .. ('\0'):rep(61 - 7);
ffi.cast('xt_setMacro_f', p.set)(obj + 380 * 2 + 4, 'Prov', str);
check('cleared', ffi.cast('xt_clear_f', p.clear)(obj, 5) == true);
local off = mem.read_uint32(p.stop + 2);
check('idle', mem.read_uint32(obj + off) == 0xFFFFFFFF);
)lua";

// stepdialog, chime, blusets' safe sender and balloon's interface flag.
const std::string kActions = kCheck + R"lua(
ffi.cdef[[
    typedef void (__thiscall* xt_OnKeyDown_f)(int32_t, int16_t, int16_t);
    typedef void (__cdecl* xt_chime_f)(int32_t, float, bool);
    typedef uint8_t (__cdecl *xt_equipex_t)(uint8_t isSubJob, uint16_t jobType, uint16_t index, uint8_t id);
]];
local func = mem.find(0, 0, '538B5C240856578B7C24148BF15753E8????????8B0D????????3BF174', 0, 0);
local this = mem.find(0, 0, '8B0D????????85C90F??????????8B410885C00F', 2, 0);
check('stepdialog found', func ~= 0 and this ~= 0);
local obj = mem.read_uint32(mem.read_uint32(this));
check('message waiting', obj ~= 0, obj);
ffi.cast('xt_OnKeyDown_f', func)(obj, 5, -1);
ffi.cast('xt_chime_f', mem.find(0, 0, 'A1????????5685C00F??????????6A006A046A40E8????????8BF083C40C8BCE', 0, 0))(0x6D696863, 1.0, true);
local eq = ffi.cast('xt_equipex_t', mem.find(0, 0, '8B0D????????81EC9C00000085C95356570F??????????8B', 0, 0));
check('equipex blu', eq(0, 0x1000, 2, 0x22) == 1);
check('equipex other', eq(0, 0x2000, 2, 1) == 0);
local hid = mem.find('FFXiMain.dll', 0, '8B4424046A016A0050B9????????E8????????F6D81BC040C3', 0, 0);
mem.write_uint8(mem.read_uint32(hid + 10) + 0xB4, 1);
)lua";

} // namespace

TEST_CASE("Ashita memory stand-ins: event message and chime and BLU sender and interface flag", "[ashita][scan]")
{
    Harness t;
    std::vector<std::pair<std::string, double>> sets;
    std::vector<std::pair<uint16_t, std::vector<std::byte>>> sent;
    xi::ashita::setClientUi({});
    xi::ashita::NativeHooks h;
    h.supports = [](std::string_view n) { return n == "event.message.waiting" || n == "event.message.key" || n == "sound.chime" || n == "interface.hidden"; };
    h.get      = [](std::string_view n) -> std::optional<double> { return n == "event.message.waiting" ? std::optional(1.0) : std::nullopt; };
    h.set      = [&](std::string_view n, double v) { sets.emplace_back(std::string(n), v); };
    xi::ashita::setNativeHooks(h);
    xi::ashita::Host host;
    host.chat = [&](const std::string& text, int) { t.chat.push_back(text); };
    host.send = [&](uint16_t id, std::vector<std::byte> p) { sent.emplace_back(id, std::move(p)); };
    t.m       = std::make_unique<AddonManager>(t.dir, Profile::Latest, xi::ashita::Resources{}, std::move(host));
    auto& blu = t.world->extendedJob[0];
    blu[0]    = 16;
    for (std::size_t k = 0; k < 20; ++k)
    {
        blu[4 + k] = static_cast<uint8_t>(k + 1);
    }
    t.addon("actions", kActions);
    REQUIRE(t.m->load("actions"));
    t.frame();
    t.frame();
    INFO(t.log());
    CHECK(t.failures().empty());
    CHECK(t.passed() == 4);
    const std::vector<std::pair<std::string, double>> want = { { "event.message.key", 5.0 }, { "sound.chime", static_cast<double>(0x6D696863) }, { "interface.hidden", 1.0 } };
    for (const auto& [k, v] : sets)
    {
        UNSCOPED_INFO(k << " = " << v);
    }
    CHECK(sets == want);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].first == 0x102);
    // The payload after the 4-byte header: SpellId, 0, 0, 0, JobId, IsSubJob, 0, 0, Spells[20] (slot 2 replaced).
    REQUIRE(sent[0].second.size() == 160);
    CHECK(std::to_integer<int>(sent[0].second[0]) == 0x22);
    CHECK(std::to_integer<int>(sent[0].second[4]) == 0x10);
    CHECK(std::to_integer<int>(sent[0].second[8]) == 1);
    CHECK(std::to_integer<int>(sent[0].second[10]) == 0x22);
    CHECK(std::to_integer<int>(sent[0].second[27]) == 20);
    xi::ashita::setNativeHooks({});
}

TEST_CASE("Ashita memory stand-ins: the macro palette through the game's functions", "[ashita][scan]")
{
    Harness t;
    std::vector<std::string> log;
    xi::ashita::MacroHooks   h;
    h.canUse = [] { return true; };
    h.name   = [](int i) { return i == 11 ? std::string("Haste") : std::string(); };
    h.line   = [](int i, int l) { return i == 11 && l == 1 ? std::string("/ma \"Haste\" <t>") : std::string(); };
    h.run    = [&](int row, int key) { log.push_back("run " + std::to_string(row) + " " + std::to_string(key)); };
    h.set    = [&](int i, const std::string& title, const std::vector<std::string>& lines) {
        std::string s = "set " + std::to_string(i) + " " + title;
        for (const auto& l : lines)
        {
            s += "|" + l;
        }
        log.push_back(s);
    };
    h.setBook = [&](int b) { log.push_back("book " + std::to_string(b)); return true; };
    h.setPage = [&](int p) { log.push_back("page " + std::to_string(p)); return true; };
    h.running = [] { return false; };
    h.stop    = [] {};
    xi::ashita::setMacroHooks(h);
    t.addon("macros", kMacros);
    REQUIRE(t.m->load("macros"));
    t.frame();
    INFO(t.log());
    CHECK(t.failures().empty());
    CHECK(t.passed() == 9);
    const std::vector<std::string> want = { "run 1 1", "book 3", "page 4", "set 2 Prov|/ja \"Provoke\" <t>|/wait 1", "set 5 " };
    CHECK(log == want);
    xi::ashita::setMacroHooks({});
}

TEST_CASE("Ashita memory stand-ins: patches become client settings", "[ashita][scan]")
{
    Harness t;
    std::map<std::string, double, std::less<>> client = { { "draw_distance.world", 1.5 } };
    std::vector<std::pair<std::string, double>> sets;
    const std::set<std::string, std::less<>> supported = { "draw_distance.world", "draw_distance.entity", "window.party.visible", "window.target.visible",
                                                           "window.alliance1.visible", "window.alliance2.visible", "chat.instant", "name_colour.0",
                                                           "chat.filter.0", "chat.filter.1", "chat.filter.2", "camera.distance.max" };
    xi::ashita::NativeHooks h;
    h.supports = [&](std::string_view n) { return supported.contains(n); };
    h.get      = [&](std::string_view n) -> std::optional<double> {
        const auto it = client.find(n);
        return it == client.end() ? std::nullopt : std::optional(it->second);
    };
    h.set = [&](std::string_view n, double v) {
        sets.emplace_back(std::string(n), v);
        client[std::string(n)] = v;
    };
    xi::ashita::setNativeHooks(h);
    t.addon("patches", kPatches);
    REQUIRE(t.m->load("patches"));
    t.frame();
    auto got = [&](const std::string& n) -> std::optional<double> {
        std::optional<double> v;
        for (const auto& [k, x] : sets)
        {
            if (k == n)
            {
                v = x;
            }
        }
        return v;
    };
    CHECK(got("draw_distance.world") == 3.0);
    CHECK(got("draw_distance.entity") == 2.0);
    CHECK(got("window.party.visible") == 0.0);
    CHECK(got("chat.instant") == 1.0);
    CHECK(got("name_colour.3") == static_cast<double>(0xC0FF0000u));
    CHECK(got("chat.filter.1") == static_cast<double>(0x1234));
    CHECK(got("camera.distance.max") == 10.0);
    CHECK_FALSE(got("window.target.visible").has_value());
    // The client changes its settings: the addons read them back.
    client["draw_distance.world"]  = 5.0;
    client["window.party.visible"] = 1.0;
    t.frame();
    t.run("/back");
    INFO(t.log());
    CHECK(t.failures().empty());
    CHECK(t.passed() == 8 + 2);
    xi::ashita::setNativeHooks({});
}

// allmaps' way: patch both key item tests to `mov eax, 1` on load, restore the bytes when the state closes.
const std::string kAllMaps = kCheck + R"lua(
local a = mem.find(0, 0, '50??????????83C4048886????????0FBF8E????????8A', 0x01, 0);
local b = mem.find(0, 0, '50??????????83C4048886????????0FBF96????????8A', 0x01, 0);
check('allmaps found', a ~= 0 and b ~= 0);
local ba, bb = mem.read_array(a, 5), mem.read_array(b, 5);
check('a call', ba[1] == 0xE8 and bb[1] == 0xE8, ba[1]);
mem.write_array(a, { 0xB8, 0x01, 0x00, 0x00, 0x00 });
mem.write_array(b, { 0xB8, 0x01, 0x00, 0x00, 0x00 });
keep = ffi.gc(ffi.cast('uint8_t*', 0), function () mem.write_array(a, ba); mem.write_array(b, bb); end);
)lua";

TEST_CASE("Ashita memory stand-ins: allmaps opens every map until it is unloaded", "[ashita][scan]")
{
    Harness t;
    std::map<std::string, double, std::less<>> client;
    xi::ashita::NativeHooks h;
    h.supports = [](std::string_view n) { return n == "map.all" || n == "map.all.view"; };
    h.get      = [&](std::string_view n) -> std::optional<double> {
        const auto it = client.find(n);
        return it == client.end() ? std::nullopt : std::optional(it->second);
    };
    h.set = [&](std::string_view n, double v) { client[std::string(n)] = v; };
    xi::ashita::setNativeHooks(h);
    t.addon("allmaps", kAllMaps);
    REQUIRE(t.m->load("allmaps"));
    t.frame();
    INFO(t.log());
    CHECK(t.failures().empty());
    CHECK(t.passed() == 2);
    CHECK(client["map.all"] == 1.0);
    CHECK(client["map.all.view"] == 1.0);
    REQUIRE(t.m->unload("allmaps"));
    CHECK(client["map.all"] == 0.0);
    CHECK(client["map.all.view"] == 0.0);
    xi::ashita::setNativeHooks({});
}

// fancychat's way with the log windows (lib/lifecycle.lua, lib/render.lua): the two log objects from the
// draw-message-window code; it reads window 1's +0x42 (its text top) and clears both +0x34 (the shown height) every
// frame to hide the game's log.
const std::string kLogObjects = kCheck + R"lua(
local p = mem.find('FFXiMain.dll', 0, 'A1????????C64059018B0D????????C6415901C20800', 0, 0);
check('log windows found', p ~= 0);
local g1, g2 = mem.read_uint32(p + 0x01), mem.read_uint32(p + 0x0B);
ashita.events.register('command', 'cmd', function (e)
    local o1, o2 = mem.read_uint32(g1), mem.read_uint32(g2);
    if (e.command == '/read') then
        print(('logs %d %d %d %d %d'):format(mem.read_int32(o1 + 0x34), bit.band(mem.read_uint32(o1 + 0x42), 0xFFFF),
            mem.read_int16(o1 + 0x4e), mem.read_int16(o1 + 0x54), mem.read_int32(o2 + 0x34)));
        e.blocked = true;
    elseif (e.command == '/poke') then
        mem.write_uint32(o1 + 0x34, 0);
        mem.write_uint32(o2 + 0x34, 0);
        e.blocked = true;
    end
end);
)lua";

TEST_CASE("Ashita memory stand-ins: an addon hides the log windows through the log objects", "[ashita][scan]")
{
    Harness                                     t;
    std::map<std::string, double, std::less<>>  client = { { "log.window1.height", 128 },
                                                           { "log.window1.max_lines", 8 },
                                                           { "log.window1.text_top", 16 + 6 + 128 - 3 },
                                                           { "log.window1.shown", 1 },
                                                           { "log.window2.height", 0 } };
    std::vector<std::pair<std::string, double>> writes;
    xi::ashita::NativeHooks                     h;
    h.supports = [](std::string_view n) { return n.starts_with("log.window"); };
    h.get      = [&](std::string_view n) -> std::optional<double> {
        const auto it = client.find(n);
        return it == client.end() ? std::nullopt : std::optional(it->second);
    };
    h.set = [&](std::string_view n, double v) { writes.emplace_back(std::string(n), v); };
    xi::ashita::setNativeHooks(h);
    t.addon("logobj", kLogObjects);
    REQUIRE(t.m->load("logobj"));
    t.frame();
    t.run("/read");
    INFO(t.log());
    CHECK(t.failures().empty());
    // The client's height, the text top 147 above the 720 screen's bottom, 8 lines of 16.
    CHECK(std::ranges::find(t.chat, "logs 128 573 8 16 0") != t.chat.end());
    CHECK(writes.empty());
    // Cleared after a frame: the next frame hands the write to the client (window 2 was 0 already).
    t.run("/poke");
    t.frame();
    REQUIRE(writes.size() == 1);
    CHECK(writes[0] == std::pair<std::string, double>{ "log.window1.height", 0 });
    // A new line grows the client's log again; the addon clears it again and the client hears it again.
    client["log.window1.height"] = 144;
    t.frame();
    t.run("/read");
    CHECK(std::ranges::find(t.chat, "logs 144 573 8 16 0") != t.chat.end());
    t.run("/poke");
    t.frame();
    REQUIRE(writes.size() == 2);
    CHECK(writes[1].second == 0);
    // Unloading lets go of the hold.
    REQUIRE(t.m->unload("logobj"));
    REQUIRE(writes.size() == 3);
    CHECK(writes[2] == std::pair<std::string, double>{ "log.window1.height", -1 });
    xi::ashita::setNativeHooks({});
}

// The `ffxi.time` library's way: find the game's clock functions and call them through function pointer casts.
const std::string kClock = kCheck + R"lua(
ffi.cdef[[
    typedef struct { int32_t sec, min, hour, mday, mon, year, wday, yday, isdst; } xi_tm_t;
    typedef uint32_t (*xi_get_f)(void);
    typedef uint32_t (*xi_fn_f)(uint32_t);
    typedef const char* (*xi_str_f)(uint32_t, uint8_t);
    typedef xi_tm_t* (*xi_tm_f)(uint32_t);
]];
local p = {
    diff = mem.find(0, 0, 'E8????????8BC88B4424043BC876??2BC1488BC8B8FFFFFF7F', 0, 0),
    now = mem.find(0, 0, 'E8????????0305????????C3', 0, 0),
    weekday = mem.find(0, 0, '8B44240433D2B9006C0000F7F1', 0, 0),
    day = mem.find(0, 0, '8B44240433D2B900950100F7F1', 0, 0),
    month = mem.find(0, 0, '8B44240433D2B900FC1200F7F1', 0, 0),
    year = mem.find(0, 0, 'B84134F81AF7642404C1EA118BC2C3', 0, 0),
    minutes = mem.find(0, 0, '8B44240433D2B990000000F7F1', 0, 0),
    hours = mem.find(0, 0, '8B44240433D2B9800D0000F7F1', 0, 0),
    moon = mem.find(0, 0, '8B4C2404B8DB4B682FF7E12BCA', 0, 0),
    moonp = mem.find(0, 0, '8B4C2404B8DB4B682FF7E18BC1', 0, 0),
    unix = mem.find(0, 0, '518D44240050E8????????8B44240483C408C3', 0, 0),
    str = mem.find(0, 0, '8B44240481EC8000000053555633F685C05774??50E8', 0, 0),
    tm = mem.find(0, 0, '8B442404568BC833F63D8017E87F894C2408', 0, 0),
};
local all = true;
for k, v in pairs(p) do if (v == 0) then all = false; print('fail ' .. k .. ' not found'); end end
check('clock found', all);
local fn = function (k) return ffi.cast('xi_fn_f', p[k]); end
local raw = ffi.cast('xi_get_f', p.now)();
check('game time', raw == 1244160 * 3 + 103680 * 4 + 3456 * 9 + 144 * 13 + 60, raw);
check('calendar', fn('year')(raw) == 3 and fn('month')(raw) == 4 and fn('day')(raw) == 9 and fn('hours')(raw) == 13 and fn('minutes')(raw) == 25,
      ('%d %d %d %d %d'):format(fn('year')(raw), fn('month')(raw), fn('day')(raw), fn('hours')(raw), fn('minutes')(raw)));
check('weekday and moon', fn('weekday')(raw) == (3 * 360 + 4 * 30 + 9) % 8 and fn('moon')(raw) == math.floor((3 * 360 + 4 * 30 + 9) / 7) % 12 and fn('moonp')(raw) <= 100);
check('time until', fn('diff')(raw + 30) == 30 and fn('diff')(raw - 30) == 0);
local u = ffi.cast('xi_get_f', p.unix)();
check('unix time', math.abs(u - os.time()) <= 2, u);
local tm = ffi.cast('xi_tm_f', p.tm)(u);
check('local time', tm ~= nil and tm.year + 1900 == tonumber(os.date('%Y')), tm ~= nil and tm.year);
local s = ffi.cast('xi_str_f', p.str)(0, 0);
check('time string', s ~= nil, 'nil');
)lua";

TEST_CASE("Ashita memory stand-ins: the game's clock functions", "[ashita][scan]")
{
    Harness t;
    xi::ashita::TimeSource src;
    src.gameTime = [] { return std::optional<uint32_t>(1244160u * 3 + 103680u * 4 + 3456u * 9 + 144u * 13 + 60u); };
    xi::ashita::setTimeSource(src);
    t.addon("clock", kClock);
    REQUIRE(t.m->load("clock"));
    t.frame();
    INFO(t.log());
    CHECK(t.failures().empty());
    CHECK(t.passed() == 8);
    xi::ashita::setTimeSource({});
}

// fancychat's targets.lua way: find the game's target helpers and call them through function pointer casts.
const std::string kTargets = kCheck + R"lua(
ffi.cdef[[
    typedef struct { uint32_t State; float x, y, z; uint32_t ActIndex : 16; uint32_t Level : 8; uint32_t unused : 8; } xi_track_t;
    typedef struct { uint8_t pad[116]; uint32_t GuideNo; uint32_t UniqueNo; } xi_actor_t;
    typedef xi_actor_t* (*xi_bt_f)(void);
    typedef const char* (*xi_name_f)(uint16_t);
    typedef const char* (*xi_teller_f)(void);
    typedef xi_track_t* (*xi_track_f)(void);
]];
local p = {
    bt = mem.find('FFXiMain.dll', 0, '66A1????????83EC186685C053565774??0FBFC08B0C85', 0, 0),
    name = mem.find('FFXiMain.dll', 0, '53568B35????????33C085F6577E??8B', 0, 0),
    r = mem.find('FFXiMain.dll', 0, '8B0D????????85C9750333C0C36A00', 0, 0),
    scan = mem.find('FFXiMain.dll', 0, 'A1????????85C074??F7', 0, 0),
};
check('targets found', p.bt ~= 0 and p.name ~= 0 and p.r ~= 0 and p.scan ~= 0);
local a = ffi.cast('xi_bt_f', p.bt)();
check('battle target', a ~= nil and a.GuideNo == 0x123 and a.UniqueNo == 0x01064123, a ~= nil and a.GuideNo);
local n = ffi.cast('xi_name_f', p.name)(0x45);
check('monster name', n ~= nil and ffi.string(n) == 'Wild Rabbit', n ~= nil and ffi.string(n));
check('no monster name', ffi.cast('xi_name_f', p.name)(0x46) == nil);
local r = ffi.cast('xi_teller_f', p.r)();
check('last teller', r ~= nil and ffi.string(r) == 'Botone', r ~= nil and ffi.string(r));
check('nothing tracked', ffi.cast('xi_track_f', p.scan)() == nil);
)lua";

TEST_CASE("Ashita memory stand-ins: the game's target helpers", "[ashita][scan]")
{
    Harness t;
    xi::ashita::TargetHooks h;
    h.battleTarget = [] { return std::optional(std::pair<uint16_t, uint32_t>(0x123, 0x01064123)); };
    h.lastTeller   = [] { return std::string("Botone"); };
    h.zoneName     = [](uint16_t i) { return i == 0x45 ? std::string("Wild Rabbit") : std::string(); };
    xi::ashita::setTargetHooks(h);
    t.addon("targets", kTargets);
    REQUIRE(t.m->load("targets"));
    t.frame();
    INFO(t.log());
    CHECK(t.failures().empty());
    CHECK(t.passed() == 6);
    xi::ashita::setTargetHooks({});
}

TEST_CASE("Ashita memory stand-ins: the inventory object carries the extended job data", "[ashita][scan]")
{
    Harness t;
    // The 'inventory' named pointer comes from config/ashita/ashita.pointers.ini, as in an Ashita install.
    std::ofstream(t.dir / "config" / "ashita" / "ashita.pointers.ini")
        << "[inventory] \r\nmodule  = FFXiMain.dll\r\npattern = A1????????8D9488????????8990????????E9\r\noffset  = 1\r\ncount   = 0\r\n";
    auto& mainJob = t.world->extendedJob[0];
    mainJob[0]    = 16; // Job
    mainJob[4]    = 1;  // SetSpells[0] (spell 513)
    mainJob[5]    = 0x29;
    mainJob[23]   = 0x7F;
    auto& subJob  = t.world->extendedJob[1];
    subJob[0]     = 16;
    subJob[1]     = 1;
    subJob[4]     = 0x55;
    t.addon("blu", kBlu);
    REQUIRE(t.m->load("blu"));
    t.frame();
    t.run("/blu");
    INFO(t.log());
    CHECK(t.failures().empty());
    CHECK(t.passed() == 4);
}

TEST_CASE("Ashita memory stand-ins: emulated signatures follow the client", "[ashita][scan]")
{
    Harness t;
    t.world->weather    = 6;
    t.world->inMogHouse = false;
    xi::ashita::setClientUi({});
    t.addon("readers", kReaders);
    REQUIRE(t.m->load("readers"));
    t.frame();
    t.run("/s1");
    t.world->inMogHouse = true;
    xi::ashita::setClientUi({ "menu map0", true, true, true });
    t.frame();
    t.run("/s2");
    INFO(t.log());
    CHECK(t.failures().empty());
    CHECK(t.passed() == 6 + 9 + 6);
}

TEST_CASE("Ashita memory stand-ins: config functions and ime through ffi", "[ashita][scan]")
{
    Harness t;
    int32_t value = 7, offline = -1;
    xi::ashita::ConfigHooks h;
    h.get = [&](int32_t id) -> std::optional<int32_t> { return id == 42 ? std::optional(value) : std::nullopt; };
    h.set = [&](int32_t id, int32_t v) {
        if (id != 42)
        {
            return false;
        }
        value = v;
        return true;
    };
    h.entry = [&](int32_t id) -> std::optional<xi::ashita::ConfigEntry> {
        if (id != 42)
        {
            return std::nullopt;
        }
        return xi::ashita::ConfigEntry{ value, 1, 0, 10, 3 };
    };
    h.autoOffline = [&](int32_t v) { offline = v; };
    xi::ashita::setConfigHooks(h);
    t.addon("cfg", kConfig);
    REQUIRE(t.m->load("cfg"));
    t.frame();
    INFO(t.log());
    CHECK(t.failures().empty());
    CHECK(t.passed() == 11);
    CHECK(offline == 1);
    xi::ashita::setConfigHooks({});
}

TEST_CASE("Ashita memory stand-ins: the game image", "[ashita][scan]")
{
    const char* root = xi::install::gameRootOrNull();
    if (!root || !fs::exists(fs::path(root) / "FFXiMain.dll"))
    {
        SKIP("FFXI_ROOT with FFXiMain.dll not set");
    }
    const auto img = xi::ashita::gameImage();
    REQUIRE(img.base != 0);
    CHECK(img.base == 0x10000000);
    // The packed code is unpacked: a code signature is found inside the image, and only there.
    const uintptr_t a = xi::ashita::findPattern("8B480C85C974??8B510885D274??3B05");
    REQUIRE(a != 0);
    CHECK(a > img.base);
    CHECK(a < img.base + img.size);
    CHECK(*reinterpret_cast<const uint8_t*>(a) == 0x8B);
    CHECK(xi::ashita::findPattern("8B480C85C974??8B510885D274??3B05", 1) == 0);
    // Its operand is a global inside the image, which reads and writes.
    uint32_t g = 0;
    std::memcpy(&g, reinterpret_cast<const void*>(a + 16), 4); // unaligned
    CHECK(xi::ashita::validRange(g, 4));
    CHECK_FALSE(xi::ashita::validRange(0, 4));
    CHECK_FALSE(xi::ashita::validRange(img.base + img.size, 4));
    // The emulated readers and config work on the real addresses too.
    Harness t;
    t.world->weather = 6;
    xi::ashita::setClientUi({});
    t.addon("readers", kReaders);
    REQUIRE(t.m->load("readers"));
    t.frame();
    t.run("/s1");
    INFO(t.log());
    CHECK(t.failures().empty());
    CHECK(t.passed() == 6 + 9);
}

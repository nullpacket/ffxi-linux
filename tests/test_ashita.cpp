// SPDX-License-Identifier: GPL-3.0-or-later
// The Ashita addon layer (src/ashita, docs/ashita-compat.md) driven headless: a temp Ashita root with small test addons
// written here, a fake host, and frames without a session. Each test addon reports through print(): "ok <check>" or
// "fail <check>: <why>"; the C++ side checks the lines, the packets and what the manager returns.
#include "ashita/ashita.h"
#include "ashita/input_map.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using xi::ashita::AddonManager;
using xi::ashita::Frame;
using xi::ashita::PacketEvent;
using xi::ashita::Profile;

namespace
{

// A fresh Ashita root under the temp directory, removed afterwards.
struct TempRoot
{
    fs::path dir;
    TempRoot()
    {
        static int n = 0;
        dir = fs::temp_directory_path() /
              ("xi_ashita_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(n++));
        fs::create_directories(dir / "addons");
        fs::create_directories(dir / "config");
        fs::create_directories(dir / "scripts");
    }
    ~TempRoot()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    TempRoot(const TempRoot&)            = delete;
    TempRoot& operator=(const TempRoot&) = delete;

    void addon(const std::string& name, const std::string& code) const
    {
        fs::create_directories(dir / "addons" / name);
        std::ofstream(dir / "addons" / name / (name + ".lua")) << code;
    }
};

struct Harness
{
    TempRoot                                            root;
    std::vector<std::string>                            chat;
    std::vector<std::pair<std::string, int>>            commands;
    std::vector<std::pair<uint16_t, std::vector<std::byte>>> sent;
    std::vector<std::vector<std::byte>>                 injected;
    std::vector<uint16_t>                               targets;
    std::shared_ptr<xi::client::WorldSnapshot>          world = std::make_shared<xi::client::WorldSnapshot>();
    std::unique_ptr<AddonManager>                       m;

    explicit Harness(Profile p = Profile::Latest)
    {
        world->inZone   = true;
        world->charName = "Tester";
        start(p);
    }
    void start(Profile p)
    {
        xi::ashita::Host h;
        h.command = [this](const std::string& line, int mode) { commands.emplace_back(line, mode); };
        h.chat    = [this](const std::string& text, int) { chat.push_back(text); };
        h.send    = [this](uint16_t id, std::vector<std::byte> payload) { sent.emplace_back(id, std::move(payload)); };
        h.inject  = [this](std::vector<std::byte> packet) { injected.push_back(std::move(packet)); };
        h.target  = [this](uint16_t index) { targets.push_back(index); };
        m         = std::make_unique<AddonManager>(root.dir, p, xi::ashita::Resources{}, std::move(h));
    }
    void frame(int n = 1)
    {
        for (int i = 0; i < n; ++i)
        {
            Frame f;
            f.world   = std::make_shared<xi::client::WorldSnapshot>(*world);
            f.windowW = 1280;
            f.windowH = 720;
            m->update(std::move(f));
        }
    }
    [[nodiscard]] bool said(const std::string& s) const
    {
        return std::find(chat.begin(), chat.end(), s) != chat.end();
    }
    [[nodiscard]] std::vector<std::string> failures() const
    {
        std::vector<std::string> v;
        for (const auto& l : chat)
        {
            // "fail ..." from the checks, and errors the prelude reports (they carry the addon name in brackets).
            if (l.rfind("fail ", 0) == 0 || l.find("stack traceback") != std::string::npos || l.find(".lua:") != std::string::npos)
            {
                v.push_back(l);
            }
        }
        return v;
    }
};

std::vector<std::byte> bytes(std::initializer_list<int> v)
{
    std::vector<std::byte> out;
    for (const int b : v)
    {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

// The check helper every test addon starts with.
const std::string kCheck = R"lua(
local function check(name, ok, why)
    if (ok) then print('ok ' .. name); else print('fail ' .. name .. ': ' .. tostring(why)); end
end
)lua";

// Where Ashita's own addons/libs can be found for the tests that use them (settings, common): XI_ASHITA_LIBS, else the
// user's Ashita checkout. Empty when there is none (those tests skip).
fs::path ashitaLibs()
{
    std::error_code ec;
    if (const char* e = std::getenv("XI_ASHITA_LIBS"); e && fs::exists(fs::path(e) / "settings.lua", ec))
    {
        return e;
    }
    if (const char* home = std::getenv("HOME"))
    {
        const fs::path p = fs::path(home) / "git" / "Ashita-v4beta" / "addons" / "libs";
        if (fs::exists(p / "settings.lua", ec))
        {
            return p;
        }
    }
    return {};
}

// An ImGui context for frames without a GPU: it claims dynamic textures (as the client's Vulkan backend does, so fonts an
// addon asks for load the same way), and renderHeadless() answers the texture requests a renderer would.
ImGuiContext* headlessImGui(ImVec2 size)
{
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO&      io  = ImGui::GetIO();
    io.DisplaySize    = size;
    io.DeltaTime      = 1.0f / 60.0f;
    io.IniFilename    = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetPlatformIO().Renderer_TextureMaxWidth  = 4096;
    ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;
    return ctx;
}

void renderHeadless()
{
    ImGui::Render();
    for (ImTextureData* t : ImGui::GetPlatformIO().Textures)
    {
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates)
        {
            t->SetTexID(static_cast<ImTextureID>(1));
            t->SetStatus(ImTextureStatus_OK);
        }
        else if (t->Status == ImTextureStatus_WantDestroy)
        {
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

} // namespace

TEST_CASE("ashita: struct.pack / struct.unpack as Ashita's struct library")
{
    Harness h;
    h.root.addon("structs", kCheck + R"lua(
local s = struct.pack('<HIb', 0x1234, 0xDEADBEEF, -1);
check('pack.size', #s == 7, #s);
check('pack.bytes', s == '\x34\x12\xEF\xBE\xAD\xDE\xFF', ('%q'):format(s));
local a, b, c, nxt = struct.unpack('<HIb', s);
check('unpack.values', a == 0x1234 and b == 0xDEADBEEF and c == -1, tostring(a) .. ' ' .. tostring(b) .. ' ' .. tostring(c));
check('unpack.next', nxt == 8, nxt);
-- Ashita's addons read packets with 1-based offsets and native (little-endian) order.
local pkt = '\x0A\x00\x00\x00\x78\x56\x34\x12';
check('unpack.offset', struct.unpack('L', pkt, 0x04 + 0x01) == 0x12345678, struct.unpack('L', pkt, 5));
local name = struct.unpack('c4', 'Abc\0rest', 1);
check('unpack.chars', name == 'Abc\0', ('%q'):format(name));
check('pack.big', struct.pack('>H', 0x0102) == '\x01\x02', 'big endian');
check('unpack.float', math.abs(struct.unpack('<f', struct.pack('<f', 1.5)) - 1.5) < 1e-6, 'float');
check('require', require('struct') == struct, 'require struct');
)lua");
    REQUIRE(h.m->load("structs"));
    CHECK(h.failures().empty());
    CHECK(h.said("ok pack.bytes"));
    CHECK(h.said("ok unpack.offset"));
    CHECK(h.said("ok require"));
    for (const auto& f : h.failures())
    {
        UNSCOPED_INFO(f);
    }
}

TEST_CASE("ashita: ashita.bits reads the game's least-significant-bit-first packing")
{
    Harness h;
    h.root.addon("bits", kCheck + R"lua(
local ffi = require('ffi');
local data = '\x34\x12\xF0';
check('be.16', ashita.bits.unpack_be(data, 0, 0, 16) == 0x1234, ashita.bits.unpack_be(data, 0, 0, 16));
check('be.3args', ashita.bits.unpack_be(data, 4, 8) == 0x23, ashita.bits.unpack_be(data, 4, 8));
check('be.offsets', ashita.bits.unpack_be(data, 2, 4, 4) == 0xF, ashita.bits.unpack_be(data, 2, 4, 4));
local raw = ffi.new('uint8_t[4]');
local nxt = ashita.bits.pack_be(raw, 0x2AB, 0, 3, 10);
check('pack.next', nxt == 13, nxt);
check('pack.roundtrip', ashita.bits.unpack_be(raw, 0, 3, 10) == 0x2AB, ashita.bits.unpack_be(raw, 0, 3, 10));
local t = { 0, 0 };
ashita.bits.pack_be(t, 5, 0, 0, 3);
check('pack.table', t[1] == 5, t[1]);
check('le.msb', ashita.bits.unpack_le('\x80', 0, 0, 1) == 1, ashita.bits.unpack_le('\x80', 0, 0, 1));
)lua");
    REQUIRE(h.m->load("bits"));
    for (const auto& f : h.failures())
    {
        UNSCOPED_INFO(f);
    }
    CHECK(h.failures().empty());
    CHECK(h.said("ok be.16"));
    CHECK(h.said("ok pack.roundtrip"));
}

TEST_CASE("ashita: command events block a line, and errors in one addon leave the others running")
{
    Harness h;
    h.root.addon("broken", R"lua(
ashita.events.register('command', 'broken_cmd', function (e) error('boom in broken'); end);
)lua");
    h.root.addon("cmds", R"lua(
ashita.events.register('command', 'cmds_cmd', function (e)
    local w = {};
    for s in e.command:gmatch('%S+') do w[#w + 1] = s; end
    if (w[1] == '/xit') then
        e.blocked = true;
        print('got ' .. (w[2] or '') .. ' mode ' .. tostring(e.mode));
        AshitaCore:GetChatManager():QueueCommand(1, '/echo queued');
    end
end);
)lua");
    REQUIRE(h.m->load("broken"));
    REQUIRE(h.m->load("cmds"));
    CHECK(h.m->command("/xit hello", 1));
    CHECK(h.said("got hello mode 1"));
    CHECK(std::any_of(h.chat.begin(), h.chat.end(), [](const auto& l) { return l.find("boom in broken") != std::string::npos; }));
    REQUIRE(h.commands.size() == 1);
    CHECK(h.commands[0].first == "/echo queued");
    // The queued line comes back as injected and is not blocked.
    CHECK_FALSE(h.m->command("/echo queued", 1));
    CHECK_FALSE(h.m->command("/say hi", 1));
}

TEST_CASE("ashita: SDL keys and buttons as the Win32 virtual keys and mouse messages")
{
    using namespace xi::ashita;
    CHECK(vkFromSdl(SDLK_LSHIFT) == kVkShift);
    CHECK(vkFromSdl(SDLK_RSHIFT) == kVkShift);
    CHECK(vkFromSdl(SDLK_LCTRL) == 0x11);
    CHECK(vkFromSdl(SDLK_RALT) == 0x12);
    CHECK(vkFromSdl(SDLK_A) == 'A');
    CHECK(vkFromSdl(SDLK_Z) == 'Z');
    CHECK(vkFromSdl(SDLK_7) == '7');
    CHECK(vkFromSdl(SDLK_F1) == 0x70);
    CHECK(vkFromSdl(SDLK_F12) == 0x7B);
    CHECK(vkFromSdl(SDLK_KP_0) == 0x60);
    CHECK(vkFromSdl(SDLK_KP_9) == 0x69);
    CHECK(vkFromSdl(SDLK_RETURN) == 0x0D);
    CHECK(vkFromSdl(SDLK_ESCAPE) == 0x1B);
    CHECK(vkFromSdl(SDLK_SLASH) == 0xBF);
    CHECK(vkFromSdl(SDLK_UNKNOWN) == 0);
    CHECK(buttonMessage(SDL_BUTTON_LEFT, true) == 0x201);
    CHECK(buttonMessage(SDL_BUTTON_LEFT, false) == 0x202);
    CHECK(buttonMessage(SDL_BUTTON_RIGHT, true) == 0x204);
    CHECK(buttonMessage(SDL_BUTTON_MIDDLE, false) == 0x208);
    CHECK(buttonMessage(SDL_BUTTON_X1, true) == 0);
}

TEST_CASE("ashita: key and mouse events drive an addon's own Shift + drag (equipmon's pattern)")
{
    Harness h;
    // equipmon's handlers, trimmed: Shift from the key event's lparam up bit, a hit test on its own box, the drag from
    // left down / move / up, blocked while it is its.
    h.root.addon("dragbox", kCheck + R"lua(
local box = { x = 100, y = 100, w = 50, h = 50, shift = false, dragging = false, dx = 0, dy = 0 };
ashita.events.register('key', 'k', function (e)
    if (e.wparam == 0x10) then
        box.shift = not (bit.band(e.lparam, bit.lshift(0x8000, 0x10)) == bit.lshift(0x8000, 0x10));
    end
end);
ashita.events.register('mouse', 'm', function (e)
    local hit = e.x >= box.x and e.x <= box.x + box.w and e.y >= box.y and e.y <= box.y + box.h;
    if (e.message == 512 and box.dragging) then
        box.x, box.y = e.x - box.dx, e.y - box.dy;
        e.blocked = true;
    elseif (e.message == 513 and hit and box.shift) then
        box.dragging, box.dx, box.dy = true, e.x - box.x, e.y - box.y;
        e.blocked = true;
    elseif (e.message == 514 and box.dragging) then
        box.dragging = false;
        e.blocked = true;
    elseif (e.message == 522 and hit) then
        print('wheel ' .. tostring(e.delta));
        e.blocked = true;
    end
end);
ashita.events.register('command', 'c', function (e)
    if (e.command == '/where') then print(('box %d %d'):format(box.x, box.y)); e.blocked = true; end
end);
)lua");
    REQUIRE(h.m->load("dragbox"));
    h.frame();
    // Without Shift the click is not the addon's.
    CHECK_FALSE(h.m->mouse(0x201, 120, 120, 0));
    CHECK_FALSE(h.m->mouse(0x202, 120, 120, 0));
    // Shift held: down on the box, moves, up; each blocked (the game does not see them).
    CHECK_FALSE(h.m->key(0x10, true, false));
    CHECK(h.m->mouse(0x201, 120, 120, 0));
    CHECK(h.m->mouse(0x200, 220, 170, 0));
    CHECK(h.m->mouse(0x202, 220, 170, 0));
    CHECK_FALSE(h.m->key(0x10, false, false));
    CHECK(h.m->command("/where", 1));
    CHECK(h.said("box 200 150"));
    // A move after the drag is not taken; the wheel over the box is, with WHEEL_DELTA a notch.
    CHECK_FALSE(h.m->mouse(0x200, 300, 300, 0));
    CHECK(h.m->mouse(0x20A, 210, 160, -120));
    CHECK(h.said("wheel -120"));
    CHECK(h.failures().empty());
}

TEST_CASE("ashita: Shift + left drag moves an unlocked primitive or font object; locked ones stay")
{
    Harness h;
    h.root.addon("prims", kCheck + R"lua(
local pm = AshitaCore:GetPrimitiveManager();
local free = pm:Create('free');
free:SetVisible(true); free:SetPositionX(10); free:SetPositionY(20); free:SetWidth(40); free:SetHeight(30);
local fixed = pm:Create('fixed');
fixed:SetVisible(true); fixed:SetPositionX(300); fixed:SetPositionY(300); fixed:SetWidth(40); fixed:SetHeight(30);
fixed:SetLocked(true);
-- Anchored to the right edge (negative x): the drag makes it absolute.
local right = pm:Create('right');
right:SetVisible(true); right:SetPositionX(-100); right:SetPositionY(500); right:SetWidth(50); right:SetHeight(50);
ashita.events.register('command', 'c', function (e)
    if (e.command == '/where') then
        print(('free %d %d fixed %d %d right %d %d'):format(free:GetPositionX(), free:GetPositionY(), fixed:GetPositionX(),
            fixed:GetPositionY(), right:GetPositionX(), right:GetPositionY()));
        e.blocked = true;
    end
end);
)lua");
    REQUIRE(h.m->load("prims"));
    h.frame(); // the window size (1280 x 720) for the right-anchored one
    // No Shift: a plain click passes to the game and moves nothing.
    CHECK_FALSE(h.m->mouse(0x201, 20, 30, 0));
    CHECK_FALSE(h.m->mouse(0x200, 60, 70, 0));
    CHECK_FALSE(h.m->mouse(0x202, 60, 70, 0));
    h.m->key(0x10, true, false);
    CHECK(h.m->mouse(0x201, 20, 30, 0)); // 10, 10 into it
    CHECK(h.m->mouse(0x200, 110, 230, 0));
    CHECK(h.m->mouse(0x202, 110, 230, 0));
    CHECK_FALSE(h.m->mouse(0x201, 310, 310, 0)); // locked
    CHECK_FALSE(h.m->mouse(0x200, 400, 400, 0));
    h.m->mouse(0x202, 400, 400, 0);
    CHECK(h.m->mouse(0x201, 1190, 510, 0)); // x 1180..1230 on a 1280-wide window
    CHECK(h.m->mouse(0x200, 600, 510, 0));
    CHECK(h.m->mouse(0x202, 600, 510, 0));
    h.m->key(0x10, false, false);
    CHECK(h.m->command("/where", 1));
    CHECK(h.said("free 100 220 fixed 300 300 right 590 500"));
    CHECK(h.failures().empty());
}

TEST_CASE("ashita: text_in changes and hides log lines; text_out changes what is said")
{
    Harness h;
    h.root.addon("texts", R"lua(
ashita.events.register('text_in', 'texts_in', function (e)
    if (e.message:find('secret')) then e.blocked = true; end
    if (e.message:find('hello')) then e.message_modified = e.message_modified:upper(); end
end);
ashita.events.register('text_out', 'texts_out', function (e)
    if (e.message == 'lol') then e.message_modified = 'laughs'; end
    if (e.message == 'nope') then e.blocked = true; end
end);
)lua");
    REQUIRE(h.m->load("texts"));
    h.frame(); // nothing in the log yet: lines from now on are new
    // The manager marks "nothing seen" with serial 1, so the process's very first ChatLine (serial 1) would never reach
    // text_in; take that serial here (reported for the layer: chatSeen_'s sentinel collides with the first serial).
    [[maybe_unused]] const xi::client::ChatLine first;
    xi::client::ChatLine a;
    a.kind = 0;
    a.from = "Someone";
    a.text = "hello there";
    xi::client::ChatLine b;
    b.kind = 4;
    b.from = "Pal";
    b.text = "the secret plan";
    xi::client::ChatLine c;
    c.kind = 0;
    c.from = "Other";
    c.text = "plain";
    h.world->chat = { a, b, c };
    h.frame();
    const auto ea = h.m->lineEdit(a.serial);
    REQUIRE(ea.has_value());
    CHECK_FALSE(ea->hidden);
    CHECK(ea->text == "SOMEONE : HELLO THERE");
    const auto eb = h.m->lineEdit(b.serial);
    REQUIRE(eb.has_value());
    CHECK(eb->hidden);
    CHECK_FALSE(h.m->lineEdit(c.serial).has_value());

    std::string line = "lol";
    CHECK_FALSE(h.m->textOut(line, 1));
    CHECK(line == "laughs");
    std::string blocked = "nope";
    CHECK(h.m->textOut(blocked, 1));
}

TEST_CASE("ashita: packet_in changes through data_modified and data_modified_raw, and blocks")
{
    Harness h;
    h.root.addon("pkta", R"lua(
local ffi = require('ffi');
ashita.events.register('packet_in', 'pkta_in', function (e)
    if (e.id == 0x0AA) then
        -- A new string replaces the bytes.
        e.data_modified = e.data_modified:sub(1, 4) .. '\x99' .. e.data_modified:sub(6);
    elseif (e.id == 0x0AB) then
        -- Written in place through the raw pointer, then blocked.
        ffi.cast('uint8_t*', e.data_modified_raw)[4] = 0x77;
        e.blocked = true;
    end
end);
)lua");
    h.root.addon("pktb", R"lua(
ashita.events.register('packet_in', 'pktb_in', function (e)
    if (e.id == 0x0AA) then
        -- The second addon sees the first one's change, and the original in e.data.
        print(('pktb sees %02X was %02X size %d'):format(e.data_modified:byte(5), e.data:byte(5), e.size));
        AshitaCore:GetPacketManager():AddOutgoingPacket(0x0B5, { 0xB5, 0x04, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04 });
        AshitaCore:GetPacketManager():AddIncomingPacket(0x017, { 0x17, 0x00, 0x00, 0x00, 0x05 });
    end
end);
)lua");
    REQUIRE(h.m->load("pkta"));
    REQUIRE(h.m->load("pktb"));

    PacketEvent e;
    e.id   = 0x0AA;
    e.data = bytes({ 0xAA, 0x04, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44 });
    CHECK(h.m->packetIn(e));
    CHECK(e.data == bytes({ 0xAA, 0x04, 0x00, 0x00, 0x99, 0x22, 0x33, 0x44 }));
    h.frame(); // print / queued output from the session thread arrives with the next frame
    CHECK(h.said("pktb sees 99 was 11 size 8"));
    REQUIRE(h.sent.size() == 1);
    CHECK(h.sent[0].first == 0x0B5);
    CHECK(h.sent[0].second == bytes({ 0x01, 0x02, 0x03, 0x04 })); // the payload after the 4-byte header
    REQUIRE(h.injected.size() == 1);
    CHECK((static_cast<int>(h.injected[0][0]) | (static_cast<int>(h.injected[0][1]) << 8)) == (0x017 | (2 << 9))); // id and size in words

    PacketEvent f;
    f.id   = 0x0AB;
    f.data = bytes({ 0xAB, 0x04, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44 });
    CHECK_FALSE(h.m->packetIn(f));
    CHECK(f.blocked);
    CHECK(f.data == bytes({ 0xAB, 0x04, 0x00, 0x00, 0x77, 0x22, 0x33, 0x44 }));

    PacketEvent g;
    g.id   = 0x015;
    g.data = bytes({ 0x15, 0x02, 0x00, 0x00 });
    const auto before = g.data;
    CHECK(h.m->packetOut(g));
    CHECK(g.data == before);
}

TEST_CASE("ashita: tasks, coroutine.sleep / sleepf and repeating tasks run with the frames")
{
    Harness h;
    h.root.addon("tasks", R"lua(
ashita.tasks.once(0, function () print('once'); end);
ashita.tasks.oncef(2, function () print('oncef'); end);
ashita.tasks.once(function ()
    print('step 1');
    coroutine.sleepf(2);
    print('step 2');
end);
local n = 0;
ashita.tasks.repeatingf(0, 3, 1, function () n = n + 1; print('rep ' .. n); end);
local ok = pcall(coroutine.sleep, 1);
print('sleep outside a task ' .. (ok and 'ran' or 'errors'));
)lua");
    REQUIRE(h.m->load("tasks"));
    CHECK(h.said("sleep outside a task errors"));
    h.frame();
    CHECK(h.said("once"));
    CHECK(h.said("step 1"));
    CHECK_FALSE(h.said("step 2"));
    CHECK_FALSE(h.said("oncef"));
    h.frame(4);
    CHECK(h.said("oncef"));
    CHECK(h.said("step 2"));
    h.frame(4);
    CHECK(h.said("rep 3"));
    CHECK_FALSE(h.said("rep 4"));
}

TEST_CASE("ashita: /addon load, list, reload, unload; load and unload events; a broken addon fails to load")
{
    Harness h;
    h.root.addon("life", R"lua(
ashita.events.register('load', 'life_load', function () print('life loaded'); end);
ashita.events.register('unload', 'life_unload', function () print('life unloaded'); end);
)lua");
    h.root.addon("syntax", "this is not lua (");
    h.root.addon("throws", "error('at load time');");

    CHECK(h.m->command("/addon load life", 1));
    CHECK(h.said("life loaded"));
    CHECK(h.m->loaded() == std::vector<std::string>{ "life" });
    CHECK(h.m->command("/addon load LIFE", 1)); // names are case-insensitive
    CHECK(h.said("[Addons] 'life' is already loaded."));
    CHECK(h.m->command("/addons", 1));
    CHECK(h.said("[Addons] Loaded addons (1): life"));
    h.chat.clear();
    CHECK(h.m->command("/addon reload life", 1));
    CHECK(h.said("life unloaded"));
    CHECK(h.said("life loaded"));
    CHECK(h.m->command("/addon unload life", 1));
    CHECK(h.m->loaded().empty());
    CHECK(h.m->command("/addon unload life", 1));
    CHECK(h.said("[Addons] 'life' is not loaded."));

    CHECK_FALSE(h.m->load("syntax"));
    CHECK_FALSE(h.m->load("throws"));
    CHECK_FALSE(h.m->load("missing"));
    CHECK(h.m->loaded().empty());
    h.frame(); // load() of a missing addon queues its message without flushing it; the next frame does
    CHECK(std::any_of(h.chat.begin(), h.chat.end(), [](const auto& l) { return l.rfind("[Addons] Failed to load addon: 'missing'", 0) == 0; }));

    // The boot script loads addons with /addon load lines and skips plugin lines.
    std::ofstream(h.root.dir / "scripts" / "default.txt") << "/load thirdparty\r\n/addon load life\r\n/wait 3\r\n";
    h.m->runBootScript();
    CHECK(h.m->loaded() == std::vector<std::string>{ "life" });
    h.m->unloadAll();
    CHECK(h.m->loaded().empty());
}

TEST_CASE("ashita: the two profiles answer their own versions; detection and parsing")
{
    for (const auto p : { Profile::Latest, Profile::HorizonXI })
    {
        Harness h(p);
        h.root.addon("ver", R"lua(
print('version ' .. __xi.version .. ' ' .. __xi.profile);
print(('addons %.2f interface %.2f'):format(ashita.addons_version, ashita.interface_version));
print('raise ' .. tostring(AshitaCore:GetPluginManager():RaiseEvent('x', 0, 0)));
)lua");
        REQUIRE(h.m->load("ver"));
        CHECK(h.said(p == Profile::HorizonXI ? "version 4.3.1.2 horizonxi" : "version 4.3.2.1 latest"));
        CHECK(h.said("addons 4.20 interface 4.30"));
        CHECK(h.said("raise nil"));
        CHECK(h.failures().empty());
    }
    TempRoot r;
    CHECK(xi::ashita::detectProfile(r.dir) == Profile::Latest);
    fs::create_directories(r.dir / "addons" / "libs" / "typedefs");
    CHECK(xi::ashita::detectProfile(r.dir) == Profile::HorizonXI);
    CHECK(xi::ashita::parseProfile("latest") == Profile::Latest);
    CHECK(xi::ashita::parseProfile("horizonxi") == Profile::HorizonXI);
    CHECK(xi::ashita::parseProfile("4.3.1.2") == Profile::HorizonXI);
    CHECK(xi::ashita::parseProfile("4.3.2.1") == Profile::Latest);
    CHECK_FALSE(xi::ashita::parseProfile("3.0").has_value());
}

TEST_CASE("ashita: files, ini configuration and unsupported calls warn instead of failing")
{
    Harness h;
    h.root.addon("files", kCheck + R"lua(
local dir = AshitaCore:GetInstallPath() .. '\\config\\addons\\files\\';
check('mkdir', ashita.fs.create_dir(dir), 'create_dir');
check('exists', ashita.fs.exists(dir), 'exists');
local f = io.open(dir .. 'a.txt', 'w'); f:write('abc'); f:close();
check('backslash.io', ashita.fs.size(dir .. 'a.txt') == 3, ashita.fs.size(dir .. 'a.txt'));
local cm = AshitaCore:GetConfigurationManager();
cm:SetValue('files_ini', 'main', 'count', 7);
check('ini.save', cm:Save('files_ini', 'files.ini'), 'save');
cm:Delete('files_ini');
check('ini.load', cm:Load('files_ini', 'files.ini'), 'load');
check('ini.get', cm:GetUInt32('files_ini', 'main', 'count', 0) == 7, cm:GetString('files_ini', 'main', 'count'));
check('unsupported', AshitaCore:GetPacketManager():QueuePacket() == false, 'QueuePacket');
check('stub', AshitaCore:GetPointerManager():SomethingNew() == 0, 'stub');
)lua");
    REQUIRE(h.m->load("files"));
    for (const auto& f : h.failures())
    {
        UNSCOPED_INFO(f);
    }
    CHECK(h.failures().empty());
    CHECK(fs::exists(h.root.dir / "config" / "addons" / "files" / "a.txt"));
    CHECK(fs::exists(h.root.dir / "config" / "files.ini"));
}

TEST_CASE("ashita: the settings library saves and reloads (needs Ashita's addons/libs)")
{
    const fs::path libs = ashitaLibs();
    if (libs.empty())
    {
        SKIP("no Ashita addons/libs (set XI_ASHITA_LIBS)");
    }
    Harness h;
    fs::create_directory_symlink(libs, h.root.dir / "addons" / "libs");
    h.root.addon("conf", kCheck + R"lua(
require('common');
local settings = require('settings');
local defaults = T{ count = 1, name = 'x', nested = T{ on = true } };
local s = settings.load(defaults);
check('defaults', s.count == 1 and s.nested.on == true, s.count);
s.count = 5;
s.name = 'saved';
settings.save();
s.count = 9;
settings.reload();
local r = settings.get();
check('reload', r.count == 5 and r.name == 'saved' and r.nested.on == true, r.count);
ashita.events.register('command', 'conf_cmd', function (e)
    if (e.command == '/conf') then e.blocked = true; print('count ' .. settings.get().count); end
end);
)lua");
    REQUIRE(h.m->load("conf"));
    for (const auto& f : h.failures())
    {
        UNSCOPED_INFO(f);
    }
    CHECK(h.failures().empty());
    CHECK(h.said("ok reload"));
    CHECK(fs::exists(h.root.dir / "config" / "addons" / "conf" / "defaults" / "settings.lua"));
    // A second load reads the file written by the first.
    CHECK(h.m->unload("conf"));
    REQUIRE(h.m->load("conf"));
    CHECK(h.m->command("/conf", 1));
    CHECK(h.said("count 5"));
}

TEST_CASE("ashita: d3d_present draws ImGui windows; a window left open does not break the frame")
{
    Harness h;
    h.root.addon("gui", R"lua(
local imgui = AshitaCore:GetGuiManager();
ashita.events.register('d3d_present', 'gui_present', function ()
    if (imgui.Begin('xi test window')) then
        imgui.Text('hello');
    end
    imgui.End();
    imgui.Begin('left open'); -- no End: the manager recovers
end);
)lua");
    REQUIRE(h.m->load("gui"));
    ImGuiContext* ctx = headlessImGui(ImVec2(1280, 720));
    for (int i = 0; i < 2; ++i)
    {
        h.frame();
        ImGui::NewFrame();
        h.m->present();
        CHECK(ImGui::FindWindowByName("xi test window") != nullptr);
        renderHeadless();
    }
    ImGui::DestroyContext(ctx);
    CHECK(h.failures().empty());
}

// ---------------------------------------------------------------------------------------------------------------------
// Smoke test of real addons (hidden: run by hand). XI_ASHITA_SMOKE_ROOT names an Ashita root with addons/ (a copy: the
// addons write their config there), XI_ASHITA_SMOKE_PROFILE "latest" / "horizonxi" (default: detected),
// XI_ASHITA_SMOKE_OUT the TSV report, XI_ASHITA_SMOKE_ONLY a single addon. Each addon is loaded alone in a fresh
// manager, run for a few frames (with ImGui frames and d3d_present), sent its own command, a packet and a log line,
// then unloaded. Report columns: addon, loaded, kind (error / missing / stub / ok), detail.
// ---------------------------------------------------------------------------------------------------------------------
namespace
{

std::string firstLine(const std::string& s)
{
    const auto  n = s.find('\n');
    std::string l = n == std::string::npos ? s : s.substr(0, n);
    // Drop the root path in front of "file.lua:12:".
    if (const auto lua = l.find(".lua:"); lua != std::string::npos)
    {
        if (const auto slash = l.rfind('/', lua); slash != std::string::npos)
        {
            l = l.substr(slash + 1);
        }
    }
    for (auto& c : l)
    {
        if (c == '\t' || static_cast<unsigned char>(c) < 0x20)
        {
            c = ' ';
        }
    }
    return l;
}

// The missing API an error names, when it names one.
std::string missingApi(const std::string& msg)
{
    static const std::regex kCall(R"(attempt to call (?:method|field|global|upvalue|local) '([^']+)')");
    static const std::regex kIndex(R"(attempt to index (?:field|global|local|upvalue) '([^']+)' \(a nil value\))");
    static const std::regex kModule(R"(module '([^']+)' not found)");
    static const std::regex kWarn(R"(not available in ffxi-linux: (\S+))");
    std::smatch             m;
    if (std::regex_search(msg, m, kWarn))
    {
        return "stub " + m[1].str();
    }
    if (std::regex_search(msg, m, kModule))
    {
        return "module " + m[1].str();
    }
    if (std::regex_search(msg, m, kCall))
    {
        return "call " + m[1].str();
    }
    if (std::regex_search(msg, m, kIndex))
    {
        return "index " + m[1].str();
    }
    return {};
}

} // namespace

TEST_CASE("ashita smoke: load every addon of a real Ashita root", "[.smoke]")
{
    const char* rootEnv = std::getenv("XI_ASHITA_SMOKE_ROOT");
    if (!rootEnv)
    {
        SKIP("XI_ASHITA_SMOKE_ROOT not set");
    }
    const fs::path root    = rootEnv;
    const char*    profEnv = std::getenv("XI_ASHITA_SMOKE_PROFILE");
    const auto     parsed  = profEnv ? xi::ashita::parseProfile(profEnv) : std::nullopt;
    const Profile  profile = parsed ? *parsed : xi::ashita::detectProfile(root);
    const char*    only    = std::getenv("XI_ASHITA_SMOKE_ONLY");
    const char*    outEnv  = std::getenv("XI_ASHITA_SMOKE_OUT");
    std::ofstream  out(outEnv ? outEnv : "ashita_smoke.tsv");

    // Log lines from the layer while one addon runs.
    std::mutex                               logM;
    std::vector<std::pair<int, std::string>> logLines;
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([&](const spdlog::details::log_msg& msg) {
        std::lock_guard l(logM);
        logLines.emplace_back(static_cast<int>(msg.level), std::string(msg.payload.data(), msg.payload.size()));
    });
    spdlog::default_logger()->sinks().push_back(sink);

    ImGuiContext* ctx = headlessImGui(ImVec2(1600, 900));

    std::vector<std::string> names;
    for (const auto& d : fs::directory_iterator(root / "addons"))
    {
        const auto n = d.path().filename().string();
        if (d.is_directory() && n != "libs" && n[0] != '.' && fs::exists(d.path() / (n + ".lua")) && (!only || n == only))
        {
            names.push_back(n);
        }
    }
    std::sort(names.begin(), names.end());

    for (const auto& name : names)
    {
        {
            std::lock_guard l(logM);
            logLines.clear();
        }
        std::vector<std::string> chat;
        xi::ashita::Host         host;
        host.chat    = [&](const std::string& t, int) { chat.push_back(t); };
        host.command = [](const std::string&, int) {};
        host.send    = [](uint16_t, std::vector<std::byte>) {};
        host.inject  = [](std::vector<std::byte>) {};
        host.target  = [](uint16_t) {};

        auto world       = std::make_shared<xi::client::WorldSnapshot>();
        world->inZone    = true;
        world->zone      = 230;
        world->charId    = 4;
        world->actIndex  = 0x400;
        world->charName  = "Tester";
        world->hp        = 300;
        world->mp        = 120;
        world->tp        = 500;
        world->hpp       = 100;
        world->mpp       = 100;
        world->mainJob   = 1;
        world->mainLevel = 30;
        world->subJob    = 6;
        world->subLevel  = 15;
        xi::client::Entity self;
        self.id    = 4;
        self.index = 0x400;
        self.name  = "Tester";
        self.hpp   = 100;
        self.isPc  = true;
        xi::client::Entity mob;
        mob.id          = 0x010E7001;
        mob.index       = 0x001;
        mob.name        = "Wild Rabbit";
        mob.hpp         = 80;
        mob.isMob       = true;
        mob.pos         = glm::vec3(5.0f, 0.0f, 5.0f);
        world->entities = { self, mob };

        auto       m      = std::make_unique<AddonManager>(root, profile, xi::ashita::Resources{}, std::move(host));
        const bool loaded = m->load(name);
        if (loaded)
        {
            for (int i = 0; i < 6; ++i)
            {
                Frame f;
                f.world   = std::make_shared<xi::client::WorldSnapshot>(*world);
                f.self    = self;
                f.windowW = 1600;
                f.windowH = 900;
                if (i >= 2)
                {
                    f.targetIndex = 0x001;
                }
                m->update(std::move(f));
                ImGui::NewFrame();
                m->present();
                renderHeadless();
                if (i == 1)
                {
                    m->command("/" + name, 1);
                    m->command("/" + name + " help", 1);
                    PacketEvent p; // a character update of zeros: addons that parse it must not fail on it
                    p.id = 0x00D;
                    p.data.assign(0x30, std::byte{ 0 });
                    p.data[0] = std::byte{ 0x0D };
                    p.data[1] = static_cast<std::byte>((p.data.size() / 4) << 1);
                    m->packetIn(p);
                    PacketEvent q;
                    q.id = 0x015;
                    q.data.assign(0x20, std::byte{ 0 });
                    q.data[0] = std::byte{ 0x15 };
                    m->packetOut(q);
                    xi::client::ChatLine line;
                    line.kind = 0;
                    line.from = "Someone";
                    line.text = "hello smoke";
                    world->chat.push_back(line);
                    std::string said = "hello";
                    m->textOut(said, 1);
                }
            }
            m->unload(name);
        }
        m.reset();

        // Errors (chat tracebacks, failed loads, logged errors) and stubbed calls.
        std::set<std::string>                            seen;
        std::vector<std::pair<std::string, std::string>> rows; // kind, detail
        auto add = [&](const std::string& kind, const std::string& msg) {
            const auto api    = missingApi(msg);
            const auto detail = api.empty() ? firstLine(msg) : api + " | " + firstLine(msg);
            const auto k      = api.empty() ? kind : (api.rfind("stub ", 0) == 0 ? std::string("stub") : std::string("missing"));
            if (seen.insert(k + detail).second)
            {
                rows.emplace_back(k, detail);
            }
        };
        for (const auto& l : chat)
        {
            if (l.find("stack traceback") != std::string::npos || l.find(".lua:") != std::string::npos || l.rfind("[Addons] Failed", 0) == 0)
            {
                add("error", l);
            }
        }
        {
            std::lock_guard l(logM);
            for (const auto& [lvl, msg] : logLines)
            {
                if (lvl == spdlog::level::err || lvl == spdlog::level::critical)
                {
                    add("error", msg);
                }
                else if (lvl == spdlog::level::warn && msg.find("not available") != std::string::npos)
                {
                    add("warn", msg);
                }
            }
        }
        if (rows.empty())
        {
            out << name << '\t' << (loaded ? 1 : 0) << "\tok\t\n";
        }
        for (const auto& [kind, detail] : rows)
        {
            out << name << '\t' << (loaded ? 1 : 0) << '\t' << kind << '\t' << detail << '\n';
        }
        out.flush();
    }
    ImGui::DestroyContext(ctx);
    auto& sinks = spdlog::default_logger()->sinks();
    sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
}

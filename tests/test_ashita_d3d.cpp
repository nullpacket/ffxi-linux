// SPDX-License-Identifier: GPL-3.0-or-later
// The Direct3D 8 / D3DX / gdifonttexture.dll stand-ins (src/ashita/d3d8_emu.*, prelude_d3d.lua), headless: textures
// from image files, ID3DXSprite quads, the 32-bit Windows `long` addons' structures assume, and gdifonts' text and
// rectangle textures. The last test uses HorizonXI's own libs / gdifonts when the install is on this machine (they are
// linked into a temp root, never copied into the repository).
#include "ashita/ashita.h"
#include "ashita/d3d8_emu.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Catch::Approx;
using xi::ashita::AddonManager;
using xi::ashita::Profile;
namespace d3d = xi::ashita::d3d;

namespace
{

struct TempRoot
{
    fs::path dir;
    TempRoot()
    {
        static int n = 0;
        dir = fs::temp_directory_path() / ("xi_ashita_d3d_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" + std::to_string(n++));
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
    TempRoot                      root;
    std::vector<std::string>      chat;
    std::unique_ptr<AddonManager> m;
    Harness()
    {
        xi::ashita::Host h;
        h.command = [](const std::string&, int) {};
        h.chat    = [this](const std::string& text, int) { chat.push_back(text); };
        h.send    = [](uint16_t, std::vector<std::byte>) {};
        h.inject  = [](std::vector<std::byte>) {};
        h.target  = [](uint16_t) {};
        m         = std::make_unique<AddonManager>(root.dir, Profile::HorizonXI, xi::ashita::Resources{}, std::move(h));
    }
    void frame()
    {
        xi::ashita::Frame f;
        auto w           = std::make_shared<xi::client::WorldSnapshot>();
        w->inZone        = true;
        f.world          = w;
        f.windowW        = 1280;
        f.windowH        = 720;
        m->update(std::move(f));
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
    [[nodiscard]] bool said(const std::string& s) const
    {
        return std::find(chat.begin(), chat.end(), s) != chat.end();
    }
    [[nodiscard]] std::string line(const std::string& prefix) const
    {
        for (const auto& l : chat)
        {
            if (l.rfind(prefix, 0) == 0)
            {
                return l;
            }
        }
        return {};
    }
};

std::string joinLines(const std::vector<std::string>& v)
{
    std::string out;
    for (const auto& l : v)
    {
        out += l + "\n";
    }
    return out;
}

// A Lua string literal of these bytes.
std::string luaBytes(const std::vector<unsigned char>& b)
{
    std::string esc;
    for (const unsigned char c : b)
    {
        esc += "\\" + std::to_string(c);
    }
    return "'" + esc + "'";
}

const std::string kCheck = R"lua(
local ffi = require('ffi');
local function check(name, ok, why)
    if (ok) then print('ok ' .. name); else print('fail ' .. name .. ': ' .. tostring(why)); end
end
)lua";

// A 32-bit bottom-up BMP of 2 x 2: top row red, black; bottom row green, blue (all opaque).
std::vector<unsigned char> bmp2x2()
{
    const uint32_t px[4] = { 0xFF00FF00u, 0xFF0000FFu, 0xFFFF0000u, 0xFF000000u }; // bottom-left first: green, blue, red, black
    std::vector<unsigned char> b(54 + 16, 0);
    const auto put32 = [&b](std::size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i)
        {
            b[at + static_cast<std::size_t>(i)] = static_cast<unsigned char>(v >> (8 * i));
        }
    };
    b[0] = 'B';
    b[1] = 'M';
    put32(2, static_cast<uint32_t>(b.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, 2);
    put32(22, 2);
    b[26] = 1;
    b[28] = 32;
    put32(34, 16);
    for (std::size_t i = 0; i < 4; ++i)
    {
        const uint32_t c = px[i]; // stored B, G, R, A
        b[54 + i * 4 + 0] = static_cast<unsigned char>(c);
        b[54 + i * 4 + 1] = static_cast<unsigned char>(c >> 8);
        b[54 + i * 4 + 2] = static_cast<unsigned char>(c >> 16);
        b[54 + i * 4 + 3] = static_cast<unsigned char>(c >> 24);
    }
    return b;
}

ImGuiContext* headlessImGui()
{
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO&      io  = ImGui::GetIO();
    io.DisplaySize    = ImVec2(1280, 720);
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

// HorizonXI's addons folder, when the install is here.
fs::path hxiAddons()
{
    if (const char* e = std::getenv("XI_HXI_ADDONS"))
    {
        return e;
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr)
    {
        return {};
    }
    // A Wine prefix under ~/Games, one or two levels down (~/Games/X, ~/Games/<launcher>/X), as game_data.cpp looks.
    const auto usable = [](const fs::path& prefix) {
        const fs::path  p = prefix / "drive_c/Program Files/HorizonXI/Game/addons";
        std::error_code ec;
        return fs::exists(p / "libs" / "d3d8.lua", ec) && fs::exists(p / "gdifonts" / "include.lua", ec) ? p
                                                                                                         : fs::path{};
    };
    std::error_code ec;
    for (const auto& a : fs::directory_iterator(fs::path(home) / "Games", ec))
    {
        if (fs::path p = usable(a.path()); !p.empty())
        {
            return p;
        }
        std::error_code ec2;
        for (const auto& b : fs::directory_iterator(a.path(), ec2))
        {
            if (fs::path p = usable(b.path()); !p.empty())
            {
                return p;
            }
        }
    }
    return {};
}

// The vtable slots of Ashita's libs/d3d8 the checks call, written out here (layouts as libs/d3d8 declares them, with
// `long` for DWORD / LONG as libs/win32types.lua has it).
const std::string kDecls = R"lua(
ffi.cdef[[
    typedef struct { long left, top, right, bottom; } XRECT;
    typedef struct { float x, y; } XVEC2;
    typedef struct { unsigned long Format, Type, Usage, Pool, Size, MultiSampleType, Width, Height; } XDESC;
    typedef struct { int Pitch; void* pBits; } XLOCKED;
    typedef struct XTEX XTEX;
    typedef struct XSPRITE XSPRITE;
    struct XTEXVtbl { void* q; unsigned long (*AddRef)(XTEX*); unsigned long (*Release)(XTEX*); void* pad[11];
        int32_t (*GetLevelDesc)(XTEX*, unsigned int, XDESC*); void* gsl; int32_t (*LockRect)(XTEX*, unsigned int, XLOCKED*, const XRECT*, unsigned long);
        int32_t (*UnlockRect)(XTEX*, unsigned int); };
    struct XTEX { struct XTEXVtbl* lpVtbl; };
    struct XSPRITEVtbl { void* q; unsigned long (*AddRef)(XSPRITE*); unsigned long (*Release)(XSPRITE*); void* gd;
        int32_t (*Begin)(XSPRITE*);
        int32_t (*Draw)(XSPRITE*, XTEX*, const XRECT*, const XVEC2*, const XVEC2*, float, const XVEC2*, unsigned long);
        void* dt; int32_t (*End)(XSPRITE*); };
    struct XSPRITE { struct XSPRITEVtbl* lpVtbl; };
]];
)lua";

} // namespace

TEST_CASE("ashita d3d8: native textures live below 4 GiB and release cleanly")
{
    const auto before = d3d::liveTextures();
    const uint32_t px[2] = { 0xFF112233u, 0x80445566u };
    const auto     id    = d3d::createTexture(2, 1, px);
    REQUIRE(id != 0);
    CHECK(id < (uint64_t{ 1 } << 32)); // addons pass tonumber(ffi.cast('uint32_t', tex)) to ImGui
    CHECK(d3d::isTexture(id));
    int w = 0, h = 0;
    REQUIRE(d3d::textureSize(id, w, h));
    CHECK(w == 2);
    CHECK(h == 1);
    CHECK(d3d::texturePixels(id)[1] == 0x80445566u);
    CHECK(d3d::liveTextures() == before + 1);
    d3d::release(id);
    CHECK_FALSE(d3d::isTexture(id));
    CHECK(d3d::liveTextures() == before);
    CHECK(reinterpret_cast<uintptr_t>(d3d::device()) < (uintptr_t{ 1 } << 32));
}

TEST_CASE("ashita d3d8: D3DX textures from files and memory, level desc, lock, sprite quads")
{
    Harness h;
    {
        const auto b = bmp2x2();
        std::ofstream(h.root.dir / "addons" / "Pic.BMP", std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
        h.root.addon("sprites", kCheck + kDecls + "local BMP = " + luaBytes(b) + R"lua(;
local dev = AshitaCore:GetDirect3DDevice();
check('device', dev ~= nil);
check('rect.size', ffi.sizeof('XRECT') == 16, ffi.sizeof('XRECT'));
check('desc.size', ffi.sizeof('XDESC') == 32, ffi.sizeof('XDESC'));

-- From a file, by a Windows path whose case differs from the file's.
local tp = ffi.new('XTEX*[1]');
local path = addon.path:gsub('/', '\\') .. '\\..\\pic.bmp';
local hr = ffi.C.D3DXCreateTextureFromFileA(dev, path, tp);
check('file.hr', hr == 0, ('%x'):format(hr));
local tex = ffi.cast('XTEX*', tp[0]);
check('file.low', tonumber(ffi.cast('uintptr_t', tex)) < 2^32, '');
local desc = ffi.new('XDESC[1]');
check('desc.hr', tex.lpVtbl.GetLevelDesc(tex, 0, desc) == 0, '');
check('desc.wh', desc[0].Width == 2 and desc[0].Height == 2 and desc[0].Format == 21, tostring(desc[0].Width) .. 'x' .. tostring(desc[0].Height));
local lock = ffi.new('XLOCKED[1]');
check('lock.hr', tex.lpVtbl.LockRect(tex, 0, lock, nil, 0) == 0, '');
local px = ffi.cast('uint32_t*', lock[0].pBits);
check('lock.pitch', lock[0].Pitch == 8, lock[0].Pitch);
check('lock.red', px[0] == 0xFFFF0000, ('%08x'):format(px[0]));
check('lock.black', px[1] == 0xFF000000, ('%08x'):format(px[1]));
check('lock.blue', px[3] == 0xFF0000FF, ('%08x'):format(px[3]));
px[0] = 0xFF00FFFF;
tex.lpVtbl.UnlockRect(tex, 0);
print(('texid %d'):format(tonumber(ffi.cast('uint32_t', tex))));

-- From memory (a Lua string, size -1) with colour key 0xFF000000: opaque black goes transparent.
local mp = ffi.new('XTEX*[1]');
hr = ffi.C.D3DXCreateTextureFromFileInMemoryEx(dev, BMP, -1, 0xFFFFFFFF, 0xFFFFFFFF, 1, 0, 21, 1, 0xFFFFFFFF, 0xFFFFFFFF, 0xFF000000, nil, nil, mp);
check('mem.hr', hr == 0, ('%x'):format(hr));
local mtex = ffi.cast('XTEX*', mp[0]);
local mlock = ffi.new('XLOCKED[1]');
mtex.lpVtbl.LockRect(mtex, 0, mlock, nil, 0);
local mpx = ffi.cast('uint32_t*', mlock[0].pBits);
check('mem.key', mpx[1] == 0 and mpx[0] == 0xFFFF0000, ('%08x %08x'):format(mpx[0], mpx[1]));
mtex.lpVtbl.UnlockRect(mtex, 0);
check('mem.release', mtex.lpVtbl.Release(mtex) == 0, '');

-- A sprite: scale (2, 3) then translate (10, 20); then a quarter turn about the origin.
local sp = ffi.new('XSPRITE*[1]');
check('sprite.hr', ffi.C.D3DXCreateSprite(dev, sp) == 0, '');
local sprite = ffi.cast('XSPRITE*', sp[0]);
local scale = ffi.new('XVEC2', { 2, 3 });
local pos = ffi.new('XVEC2', { 10, 20 });
local src = ffi.new('XRECT', { 0, 0, 2, 2 });
sprite.lpVtbl.Begin(sprite);
sprite.lpVtbl.Draw(sprite, tex, src, scale, nil, 0.0, pos, 0x80FF0000);
sprite.lpVtbl.Draw(sprite, tex, nil, nil, nil, math.pi / 2, nil, 0xFFFFFFFF);
sprite.lpVtbl.End(sprite);
print('drawn');
)lua");
    }
    h.m->load("sprites");
    h.frame();
    INFO(joinLines(h.chat));
    CHECK(h.failures().empty());
    CHECK(h.said("drawn"));
    const auto& q = d3d::lastQuads();
    REQUIRE(q.size() == 2);
    CHECK(q[0].x[0] == Approx(10));
    CHECK(q[0].y[0] == Approx(20));
    CHECK(q[0].x[2] == Approx(14));
    CHECK(q[0].y[2] == Approx(26));
    CHECK(q[0].u[2] == Approx(1));
    CHECK(q[0].colour == 0x80FF0000u);
    // Quarter turn: the top-right corner (2, 0) goes to (0, 2).
    CHECK(q[1].x[1] == Approx(0).margin(1e-4));
    CHECK(q[1].y[1] == Approx(2));
    // The pixel the addon wrote through LockRect reached the texture.
    const auto l  = h.line("texid ");
    REQUIRE(!l.empty());
    const auto id = static_cast<uint64_t>(std::stoull(l.substr(6)));
    REQUIRE(d3d::isTexture(id));
    CHECK(d3d::texturePixels(id)[0] == 0xFF00FFFFu);
}

TEST_CASE("ashita d3d8: imgui.Image and the draw lists take a texture number; sprites draw at End inside a frame")
{
    ImGuiContext* ctx = headlessImGui();
    {
        Harness h;
        h.root.addon("imgtex", kCheck + kDecls + "local BMP = " + luaBytes(bmp2x2()) + R"lua(;
local imgui = AshitaCore:GetGuiManager();
local dev = AshitaCore:GetDirect3DDevice();
check('bad.data', ffi.C.D3DXCreateTextureFromFileInMemory(dev, 'not an image', 0, ffi.new('XTEX*[1]')) ~= 0, '');
local tp = ffi.new('XTEX*[1]');
check('mem.hr', ffi.C.D3DXCreateTextureFromFileInMemory(dev, BMP, #BMP, tp) == 0, '');
local tex = ffi.cast('XTEX*', tp[0]);
local id = tonumber(ffi.cast('uint32_t', tex));
local sp = ffi.new('XSPRITE*[1]');
ffi.C.D3DXCreateSprite(dev, sp);
local sprite = ffi.cast('XSPRITE*', sp[0]);
local pos = ffi.new('XVEC2', { 300, 200 });
ashita.events.register('d3d_present', 'imgtex', function ()
    imgui.SetNextWindowPos({ 10, 10 });
    if (imgui.Begin('imgtex')) then
        imgui.Image(id, { 16, 16 });
        imgui.Image(tex, { 16, 16 }, { 0, 0 }, { 1, 1 });
        imgui.ImageWithBg(id, { 8, 8 }, { 0, 0 }, { 1, 1 }, { 0, 0, 0, 1 }, { 1, 1, 1, 1 });
        imgui.ImageButton('btn', id, { 8, 8 });
        imgui.GetWindowDrawList():AddImage(id, { 0, 0 }, { 4, 4 }, { 0, 0 }, { 1, 1 }, 0xFFFFFFFF);
        imgui.GetForegroundDrawList():AddImageRounded(id, { 0, 0 }, { 4, 4 }, { 0, 0 }, { 1, 1 }, 0xFFFFFFFF, 2, 0);
    end
    imgui.End();
    sprite.lpVtbl.Begin(sprite);
    sprite.lpVtbl.Draw(sprite, tex, nil, nil, nil, 0.0, pos, 0xFFFFFFFF);
    sprite.lpVtbl.End(sprite);
    print('frame');
end);
)lua");
        h.m->load("imgtex");
        h.frame();
        ImGui::NewFrame();
        h.m->present();
        // The sprite went on the foreground list as one textured quad at (300, 200).
        const auto& q = d3d::lastQuads();
        REQUIRE(q.size() == 1);
        CHECK(q[0].x[0] == Approx(300));
        renderHeadless();
        INFO(joinLines(h.chat));
        CHECK(h.failures().empty());
        CHECK(h.said("frame"));
        // The texture became an ImGui user texture (2 x 2) the renderer was asked to create.
        bool found = false;
        for (ImTextureData* t : ImGui::GetPlatformIO().Textures)
        {
            found = found || (t->Width == 2 && t->Height == 2 && t->Status == ImTextureStatus_OK);
        }
        CHECK(found);
        h.m->unloadAll();
        // Released with its addon (the GC runs Release): gone from the renderer's list after the next frames.
        for (int i = 0; i < 3; ++i)
        {
            ImGui::NewFrame();
            d3d::sweep();
            renderHeadless();
        }
        d3d::sweep();
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("ashita d3d8: fancychat's gdifonttexture build: colour marks and its own font folder")
{
    ImGuiContext* ctx = headlessImGui();
    {
        Harness h;
        h.root.addon("gdicolour", kCheck + R"lua(
ffi.cdef[[
    typedef struct IDirect3DTexture8 IDirect3DTexture8;
    typedef struct { int32_t BoxHeight, BoxWidth; float FontHeight, OutlineWidth; uint32_t FontFlags, FontColor, OutlineColor,
        GradientStyle, GradientColor; char FontFamily[256]; char FontText[4096]; } GdiFontData_t;
    typedef struct { int32_t Width; int32_t Height; IDirect3DTexture8* Texture; } GdiFontReturn_t;
]];
local r = ffi.load('gdifonttexture.dll');
if (not r.GetFontAvailable('sans')) then print('nofont'); return; end
r.SetFontFolder('/nonexistent/fancychat/gdifonts/');
local m = r.CreateFontManager(nil);
local function make(fn, text)
    local d = ffi.new('GdiFontData_t');
    d.FontHeight, d.FontColor, d.FontFamily, d.FontText = 20, 0xFFFFFFFF, 'sans', text;
    local t = r[fn](m, d);
    return t.Width, t.Height, (t.Texture ~= nil) and tonumber(ffi.cast('uint32_t', t.Texture)) or 0;
end
local pw = make('CreateTexture', 'AB');
local cw, _, cid = make('CreateTextureColor', '\\\194\167FFFF0000\195\167\\AB\\\194\167--------\195\167\\');
check('marks not drawn', cw == pw, cw .. ' vs ' .. pw);
print(('colour %d'):format(cid));
)lua");
        h.m->load("gdicolour");
        h.frame();
        INFO(joinLines(h.chat));
        if (h.said("nofont"))
        {
            ImGui::DestroyContext(ctx);
            SKIP("no fontconfig sans font");
        }
        CHECK(h.failures().empty());
        CHECK(h.said("ok marks not drawn"));
        const auto line = h.line("colour ");
        REQUIRE(!line.empty());
        const auto id = static_cast<uint64_t>(std::stoull(line.substr(7)));
        REQUIRE(d3d::isTexture(id));
        int w = 0, hh = 0;
        REQUIRE(d3d::textureSize(id, w, hh));
        const uint32_t* px  = d3d::texturePixels(id);
        int             red = 0, white = 0;
        for (int i = 0; i < w * hh; ++i)
        {
            const uint32_t c = px[i];
            red += ((c >> 24) > 200 && (c & 0xFFFFFF) == 0xFF0000) ? 1 : 0;
            white += ((c >> 24) > 200 && (c & 0xFFFFFF) == 0xFFFFFF) ? 1 : 0;
        }
        CHECK(red > 20);   // the run is drawn in the mark's colour ...
        CHECK(white == 0); // ... and nothing in FontColor
    }
    ImGui::DestroyContext(ctx);
}

TEST_CASE("ashita d3d8: HorizonXI's gdifonts render text and rectangles through the stand-in DLL")
{
    const fs::path hxi = hxiAddons();
    if (hxi.empty())
    {
        SKIP("HorizonXI's addons (libs, gdifonts) are not on this machine");
    }
    ImGuiContext* ctx = headlessImGui();
    {
        Harness h;
        fs::create_directory_symlink(hxi / "libs", h.root.dir / "addons" / "libs");
        fs::create_directory_symlink(hxi / "gdifonts", h.root.dir / "addons" / "gdifonts");
        h.root.addon("gditest", kCheck + R"lua(
require('common');
local gdi = require('gdifonts.include');
check('font.available', gdi:get_font_available('Arial') == true, '');
local obj = gdi:create_object({ text = 'Hello World', font_family = 'Arial', font_height = 18, font_color = 0xFFFFFFFF,
    outline_color = 0xFF000000, outline_width = 2, position_x = 100, position_y = 50 });
local tex, rect = obj:get_texture();
check('text.texture', tex ~= nil, 'nil');
if (rect ~= nil) then
    print(('text %d %d %d'):format(rect.right, rect.bottom, tonumber(ffi.cast('uint32_t', tex))));
end
local r = gdi:create_rect({ width = 60, height = 20, corner_rounding = 5, fill_color = 0xFF336699, outline_color = 0xFFFFFFFF,
    outline_width = 1, position_x = 10, position_y = 10 });
local rt, rr = r:get_texture();
check('rect.texture', rt ~= nil and rr.right == 60 and rr.bottom == 20, rr and (rr.right .. 'x' .. rr.bottom) or 'nil');
if (rt ~= nil) then print(('rect %d'):format(tonumber(ffi.cast('uint32_t', rt)))); end
print('made');
)lua");
        h.m->load("gditest");
        h.frame();
        ImGui::NewFrame();
        h.m->present(); // gdifonts draws its objects on d3d_present
        const auto quads = d3d::lastQuads();
        renderHeadless();
        INFO(joinLines(h.chat));
        CHECK(h.failures().empty());
        REQUIRE(h.said("made"));
        // "Hello World" at 18 px with a 2 px outline: some 90..130 x 18..26 pixels, white inside black.
        const auto t = h.line("text ");
        REQUIRE(!t.empty());
        int      tw = 0, th = 0;
        uint64_t tid = 0;
        std::sscanf(t.c_str(), "text %d %d %lu", &tw, &th, &tid);
        CHECK(tw > 80);
        CHECK(tw < 140);
        CHECK(th > 16);
        CHECK(th < 30);
        REQUIRE(d3d::isTexture(tid));
        int w = 0, hh = 0;
        REQUIRE(d3d::textureSize(tid, w, hh));
        const uint32_t* px    = d3d::texturePixels(tid);
        int             white = 0, black = 0;
        for (int i = 0; i < w * hh; ++i)
        {
            const uint32_t c = px[i];
            if ((c >> 24) > 200 && (c & 0xFFFFFF) == 0xFFFFFF)
            {
                ++white;
            }
            if ((c >> 24) > 200 && (c & 0xFFFFFF) == 0)
            {
                ++black;
            }
        }
        CHECK(white > 50);
        CHECK(black > 30);
        // The rectangle: corners transparent (rounded), the middle the fill colour.
        const auto r = h.line("rect ");
        REQUIRE(!r.empty());
        const auto rid = static_cast<uint64_t>(std::stoull(r.substr(5)));
        REQUIRE(d3d::isTexture(rid));
        const uint32_t* rp = d3d::texturePixels(rid);
        CHECK((rp[0] >> 24) < 64);
        CHECK(rp[10 * 60 + 30] == 0xFF336699u);
        // Both objects were drawn at d3d_present: the text at (100, 50).
        bool atText = false;
        for (const auto& q : quads)
        {
            atText = atText || (q.texture == tid && q.x[0] == Approx(100) && q.y[0] == Approx(50));
        }
        CHECK(atText);
    }
    ImGui::DestroyContext(ctx);
}

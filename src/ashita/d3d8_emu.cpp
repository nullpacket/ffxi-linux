// SPDX-License-Identifier: GPL-3.0-or-later
// Direct3D 8 / D3DX 8 / gdifonttexture.dll stand-ins for Ashita addons. See d3d8_emu.h and docs/ashita-compat.md
// "Direct3D 8 and gdifonts".
//
// Layouts follow Ashita's libs/d3d8 declarations as prelude_d3d.lua makes LuaJIT read them: LONG / DWORD are 32-bit
// (Windows), pointers 64-bit. Vtable slots are the ones libs/d3d8/interfaces/*.lua and d3dx8.lua declare; a slot this
// file does not implement answers E_NOTIMPL.
#include "ashita/d3d8_emu.h"

#include "ashita/lua_util.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/spdlog.h>

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <unordered_set>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wduplicated-branches"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#include <stb_image.h>
#define STB_RECT_PACK_IMPLEMENTATION
#define STBRP_STATIC
#include <imstb_rectpack.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>
#pragma GCC diagnostic pop

namespace xi::ashita
{

namespace
{

using Fn = void (*)();

constexpr int32_t kOk          = 0;
constexpr int32_t kNotImpl     = static_cast<int32_t>(0x80004001u);
constexpr int32_t kNoInterface = static_cast<int32_t>(0x80004002u);
constexpr int32_t kFail        = static_cast<int32_t>(0x80004005u);
constexpr int32_t kInvalidCall = static_cast<int32_t>(0x8876086Cu); // D3DERR_INVALIDCALL
constexpr int32_t kInvalidData = static_cast<int32_t>(0x88760B59u); // D3DXERR_INVALIDDATA

constexpr uint32_t kFmtA8R8G8B8 = 21;
constexpr uint32_t kFmtX8R8G8B8 = 22;
constexpr uint32_t kD3dxDefault = 0xFFFFFFFFu; // D3DX_DEFAULT; 0xFFFFFFFE is D3DX_DEFAULT_NONPOW2 in later D3DX

// Win32 / D3D8 structures as the addons declare them (32-bit LONG / DWORD).
struct Rect
{
    int32_t left, top, right, bottom;
};
struct Vec2
{
    float x, y;
};
struct Matrix
{
    float m[4][4];
};
struct SurfaceDesc
{
    uint32_t Format, Type, Usage, Pool, Size, MultiSampleType, Width, Height;
};
struct LockedRect
{
    int32_t Pitch;
    void*   pBits;
};
struct Viewport
{
    uint32_t X, Y, Width, Height;
    float    MinZ, MaxZ;
};
struct DisplayMode
{
    uint32_t Width, Height, RefreshRate, Format;
};
struct ImageInfo // D3DXIMAGE_INFO (D3DX 8)
{
    uint32_t Width, Height, Depth, MipLevels, Format, ResourceType, ImageFileFormat;
};
struct GdiFontData // gdifonts/include.lua
{
    int32_t  BoxHeight;
    int32_t  BoxWidth;
    float    FontHeight;
    float    OutlineWidth;
    uint32_t FontFlags;
    uint32_t FontColor;
    uint32_t OutlineColor;
    uint32_t GradientStyle;
    uint32_t GradientColor;
    char     FontFamily[256];
    char     FontText[4096];
};
struct GdiRectData
{
    int32_t  Width;
    int32_t  Height;
    int32_t  Diameter;
    uint32_t OutlineColor;
    uint32_t OutlineWidth;
    uint32_t FillColor;
    uint32_t GradientStyle;
    uint32_t GradientColor;
};

// ---------------------------------------------------------------------------------------------------------------------
// Objects. Every COM object is a Com header in memory below 4 GiB; its vtable and its state live anywhere.
// ---------------------------------------------------------------------------------------------------------------------
enum class Kind : uint32_t
{
    Device = 1,
    Texture,
    Sprite,
    FontManager,
};

struct Com
{
    const Fn* vtbl;
    Kind      kind;
    uint32_t  refs;
    void*     impl;
};

struct TextureImpl
{
    int                   w = 0, h = 0;
    std::vector<uint32_t> argb;
    ImTextureData*        data  = nullptr;
    bool                  dirty = true;
};

struct SpriteImpl
{
    bool                   begun = false;
    std::vector<d3d::Quad> quads;
};

// Headers come from 64 KiB MAP_32BIT blocks, 32 bytes a slot.
class LowPool
{
public:
    Com* get()
    {
        if (free_.empty())
        {
            grow();
        }
        if (free_.empty())
        {
            return nullptr;
        }
        Com* c = free_.back();
        free_.pop_back();
        std::memset(static_cast<void*>(c), 0, sizeof(Com));
        live_.insert(c);
        return c;
    }
    void put(Com* c)
    {
        live_.erase(c);
        std::memset(static_cast<void*>(c), 0, sizeof(Com));
        free_.push_back(c);
    }
    [[nodiscard]] bool live(uint64_t id) const
    {
        return id != 0 && live_.contains(reinterpret_cast<Com*>(static_cast<uintptr_t>(id)));
    }
    [[nodiscard]] std::size_t count(Kind k) const
    {
        return static_cast<std::size_t>(std::count_if(live_.begin(), live_.end(), [k](const Com* c) { return c->kind == k; }));
    }

private:
    void grow()
    {
        constexpr std::size_t kBlock = 0x10000, kSlot = 32;
        void* p = mmap(nullptr, kBlock, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p == MAP_FAILED)
        {
            spdlog::error("ashita: d3d8: no memory below 4 GiB for D3D objects");
            return;
        }
        auto* base = static_cast<unsigned char*>(p);
        for (std::size_t off = kBlock; off >= kSlot; off -= kSlot)
        {
            free_.push_back(reinterpret_cast<Com*>(base + off - kSlot));
        }
    }
    std::vector<Com*>        free_;
    std::unordered_set<Com*> live_;
};

LowPool& pool()
{
    static LowPool p;
    return p;
}

Com* asCom(const void* p)
{
    return const_cast<Com*>(static_cast<const Com*>(p));
}
TextureImpl* texOf(uint64_t id)
{
    if (!pool().live(id))
    {
        return nullptr;
    }
    Com* c = reinterpret_cast<Com*>(static_cast<uintptr_t>(id));
    return c->kind == Kind::Texture ? static_cast<TextureImpl*>(c->impl) : nullptr;
}
TextureImpl* texOf(const void* p)
{
    return texOf(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)));
}

std::vector<ImTextureData*>& pendingDestroy()
{
    static std::vector<ImTextureData*> v;
    return v;
}
std::vector<d3d::Quad>& lastQuadsRef()
{
    static std::vector<d3d::Quad> v;
    return v;
}

bool imguiFrame()
{
    const ImGuiContext* g = ImGui::GetCurrentContext();
    return g != nullptr && g->WithinFrameScope;
}

// The texture's ImGui side: made (and registered) the first time a draw needs it, refreshed when its pixels change.
std::optional<ImTextureRef> texRef(TextureImpl& t)
{
    if (ImGui::GetCurrentContext() == nullptr || t.w <= 0 || t.h <= 0)
    {
        return std::nullopt;
    }
    const auto fill = [&t] {
        auto* dst = t.data->Pixels;
        for (std::size_t i = 0; i < t.argb.size(); ++i)
        {
            const uint32_t c = t.argb[i];
            dst[i * 4 + 0]   = static_cast<unsigned char>(c >> 16);
            dst[i * 4 + 1]   = static_cast<unsigned char>(c >> 8);
            dst[i * 4 + 2]   = static_cast<unsigned char>(c);
            dst[i * 4 + 3]   = static_cast<unsigned char>(c >> 24);
        }
        t.data->UseColors = true;
        t.data->UsedRect  = { 0, 0, static_cast<unsigned short>(t.w), static_cast<unsigned short>(t.h) };
    };
    if (t.data == nullptr)
    {
        t.data = IM_NEW(ImTextureData)();
        t.data->Create(ImTextureFormat_RGBA32, t.w, t.h);
        fill();
        ImGui::RegisterUserTexture(t.data);
        t.dirty = false;
    }
    else if (t.dirty)
    {
        fill();
        if (t.data->Status == ImTextureStatus_OK)
        {
            const ImTextureRect all{ 0, 0, static_cast<unsigned short>(t.w), static_cast<unsigned short>(t.h) };
            t.data->UpdateRect = all;
            t.data->Updates.push_back(all);
            t.data->SetStatus(ImTextureStatus_WantUpdates);
        }
        t.dirty = false;
    }
    return t.data->GetTexRef();
}

void destroyTexture(Com* c)
{
    auto* t = static_cast<TextureImpl*>(c->impl);
    if (t->data != nullptr)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            IM_DELETE(t->data);
        }
        else if (t->data->Status == ImTextureStatus_WantCreate && t->data->TexID == ImTextureID_Invalid)
        {
            ImGui::UnregisterUserTexture(t->data); // never reached the backend
            IM_DELETE(t->data);
        }
        else
        {
            t->data->WantDestroyNextFrame = true; // may still be drawn this frame; sweep() frees it after the backend
            pendingDestroy().push_back(t->data);
        }
    }
    delete t;
}

void destroy(Com* c)
{
    switch (c->kind)
    {
        case Kind::Texture: destroyTexture(c); break;
        case Kind::Sprite: delete static_cast<SpriteImpl*>(c->impl); break;
        case Kind::Device:
        case Kind::FontManager: break;
    }
    pool().put(c);
}

// ---------------------------------------------------------------------------------------------------------------------
// IUnknown and stubs
// ---------------------------------------------------------------------------------------------------------------------
int32_t stubNotImpl()
{
    return kNotImpl;
}
int32_t queryInterface(void*, const void*, void** out)
{
    if (out != nullptr)
    {
        *out = nullptr;
    }
    return kNoInterface;
}
uint32_t addRef(void* self)
{
    return ++asCom(self)->refs;
}
uint32_t release(void* self)
{
    Com* c = asCom(self);
    if (!pool().live(reinterpret_cast<uintptr_t>(c)) || c->refs == 0)
    {
        return 0;
    }
    if (--c->refs == 0 && c->kind != Kind::Device)
    {
        destroy(c);
        return 0;
    }
    return c->refs;
}

template <std::size_t N>
std::array<Fn, N> stubTable()
{
    std::array<Fn, N> t{};
    t.fill(reinterpret_cast<Fn>(&stubNotImpl));
    t[0] = reinterpret_cast<Fn>(&queryInterface);
    t[1] = reinterpret_cast<Fn>(&addRef);
    t[2] = reinterpret_cast<Fn>(&release);
    return t;
}

Com* newCom(Kind k, const Fn* vtbl, void* impl)
{
    Com* c = pool().get();
    if (c == nullptr)
    {
        return nullptr;
    }
    c->vtbl = vtbl;
    c->kind = k;
    c->refs = 1;
    c->impl = impl;
    return c;
}

// ---------------------------------------------------------------------------------------------------------------------
// IDirect3DDevice8: the slots addons and libraries reach.
// ---------------------------------------------------------------------------------------------------------------------
std::pair<uint32_t, uint32_t> screenSize()
{
    if (const ImGuiContext* g = ImGui::GetCurrentContext(); g != nullptr && g->IO.DisplaySize.x > 0)
    {
        return { static_cast<uint32_t>(g->IO.DisplaySize.x), static_cast<uint32_t>(g->IO.DisplaySize.y) };
    }
    return { 1280, 720 };
}
int32_t devTestCooperativeLevel(void*)
{
    return kOk;
}
uint32_t devAvailableTextureMem(void*)
{
    return 512u << 20;
}
int32_t devGetDisplayMode(void*, DisplayMode* m)
{
    if (m == nullptr)
    {
        return kInvalidCall;
    }
    const auto [w, h] = screenSize();
    *m                = { w, h, 60, kFmtX8R8G8B8 };
    return kOk;
}
Com* makeTexture(int w, int h, const uint32_t* argb);
int32_t devCreateTexture(void*, uint32_t w, uint32_t h, uint32_t, uint32_t, uint32_t, uint32_t, void** out)
{
    if (out == nullptr || w == 0 || h == 0 || w > 8192 || h > 8192)
    {
        return kInvalidCall;
    }
    Com* c = makeTexture(static_cast<int>(w), static_cast<int>(h), nullptr);
    *out   = c;
    return c != nullptr ? kOk : kFail;
}
int32_t devScene(void*)
{
    return kOk;
}
int32_t devGetViewport(void*, Viewport* v)
{
    if (v == nullptr)
    {
        return kInvalidCall;
    }
    const auto [w, h] = screenSize();
    *v                = { 0, 0, w, h, 0.0f, 1.0f };
    return kOk;
}

const std::array<Fn, 97>& deviceVtbl()
{
    static const std::array<Fn, 97> t = [] {
        auto v = stubTable<97>();
        v[3]   = reinterpret_cast<Fn>(&devTestCooperativeLevel);
        v[4]   = reinterpret_cast<Fn>(&devAvailableTextureMem);
        v[8]   = reinterpret_cast<Fn>(&devGetDisplayMode);
        v[20]  = reinterpret_cast<Fn>(&devCreateTexture);
        v[34]  = reinterpret_cast<Fn>(&devScene); // BeginScene
        v[35]  = reinterpret_cast<Fn>(&devScene); // EndScene
        v[41]  = reinterpret_cast<Fn>(&devGetViewport);
        return v;
    }();
    return t;
}

Com* deviceCom()
{
    static Com* d = newCom(Kind::Device, deviceVtbl().data(), nullptr);
    return d;
}

// ---------------------------------------------------------------------------------------------------------------------
// IDirect3DTexture8
// ---------------------------------------------------------------------------------------------------------------------
int32_t texGetDevice(void*, void** out)
{
    if (out == nullptr)
    {
        return kInvalidCall;
    }
    *out = deviceCom();
    addRef(deviceCom());
    return kOk;
}
uint32_t texZero(void*)
{
    return 0;
}
void texPreLoad(void*) {}
uint32_t texGetType(void*)
{
    return 3; // D3DRTYPE_TEXTURE
}
uint32_t texLevelCount(void*)
{
    return 1;
}
int32_t texGetLevelDesc(void* self, uint32_t level, SurfaceDesc* d)
{
    auto* t = static_cast<TextureImpl*>(asCom(self)->impl);
    if (d == nullptr || level != 0)
    {
        return kInvalidCall;
    }
    *d = { kFmtA8R8G8B8, 1 /* D3DRTYPE_SURFACE */, 0, 1 /* D3DPOOL_MANAGED */, static_cast<uint32_t>(t->w * t->h * 4), 0,
           static_cast<uint32_t>(t->w), static_cast<uint32_t>(t->h) };
    return kOk;
}
int32_t texLockRect(void* self, uint32_t level, LockedRect* lr, const Rect* r, uint32_t)
{
    auto* t = static_cast<TextureImpl*>(asCom(self)->impl);
    if (lr == nullptr || level != 0)
    {
        return kInvalidCall;
    }
    lr->Pitch      = t->w * 4;
    const int x    = r != nullptr ? std::clamp(r->left, 0, t->w) : 0;
    const int y    = r != nullptr ? std::clamp(r->top, 0, t->h) : 0;
    lr->pBits      = t->argb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(t->w) + static_cast<std::size_t>(x);
    return kOk;
}
int32_t texUnlockRect(void* self, uint32_t)
{
    static_cast<TextureImpl*>(asCom(self)->impl)->dirty = true;
    return kOk;
}
int32_t texOk(void*)
{
    return kOk;
}

const std::array<Fn, 19>& textureVtbl()
{
    static const std::array<Fn, 19> t = [] {
        auto v = stubTable<19>();
        v[3]   = reinterpret_cast<Fn>(&texGetDevice);
        v[7]   = reinterpret_cast<Fn>(&texZero);  // SetPriority
        v[8]   = reinterpret_cast<Fn>(&texZero);  // GetPriority
        v[9]   = reinterpret_cast<Fn>(&texPreLoad);
        v[10]  = reinterpret_cast<Fn>(&texGetType);
        v[11]  = reinterpret_cast<Fn>(&texZero);  // SetLOD
        v[12]  = reinterpret_cast<Fn>(&texZero);  // GetLOD
        v[13]  = reinterpret_cast<Fn>(&texLevelCount);
        v[14]  = reinterpret_cast<Fn>(&texGetLevelDesc);
        v[16]  = reinterpret_cast<Fn>(&texLockRect);
        v[17]  = reinterpret_cast<Fn>(&texUnlockRect);
        v[18]  = reinterpret_cast<Fn>(&texOk);    // AddDirtyRect
        return v;
    }();
    return t;
}

Com* makeTexture(int w, int h, const uint32_t* argb)
{
    auto* t = new TextureImpl;
    t->w    = w;
    t->h    = h;
    t->argb.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0u);
    if (argb != nullptr)
    {
        std::memcpy(t->argb.data(), argb, t->argb.size() * 4);
    }
    Com* c = newCom(Kind::Texture, textureVtbl().data(), t);
    if (c == nullptr)
    {
        delete t;
    }
    return c;
}

// ---------------------------------------------------------------------------------------------------------------------
// ID3DXSprite: Draw / DrawTransform queue quads; End puts them on ImGui's foreground list (above the game's interface,
// as Ashita's d3d_present drawing is), one AddImageQuad each, in order.
// ---------------------------------------------------------------------------------------------------------------------
ImU32 imColour(uint32_t argb)
{
    const uint32_t a = argb >> 24, r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    return IM_COL32(r, g, b, a);
}

int32_t spriteGetDevice(void* self, void** out)
{
    (void)self;
    return texGetDevice(nullptr, out);
}
int32_t spriteBegin(void* self)
{
    auto* s  = static_cast<SpriteImpl*>(asCom(self)->impl);
    s->begun = true;
    s->quads.clear();
    return kOk;
}

// The quad of a source rectangle (or the whole texture) under a 2D affine transform.
template <class Tf>
bool quadFor(void* tex, const Rect* src, uint32_t colour, d3d::Quad& q, Tf&& tf)
{
    TextureImpl* t = texOf(tex);
    if (t == nullptr || t->w <= 0 || t->h <= 0)
    {
        return false;
    }
    const Rect r  = src != nullptr ? *src : Rect{ 0, 0, t->w, t->h };
    const float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    const float lx[4] = { 0, w, w, 0 }, ly[4] = { 0, 0, h, h };
    const float u0 = static_cast<float>(r.left) / static_cast<float>(t->w), u1 = static_cast<float>(r.right) / static_cast<float>(t->w);
    const float v0 = static_cast<float>(r.top) / static_cast<float>(t->h), v1 = static_cast<float>(r.bottom) / static_cast<float>(t->h);
    const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
    for (int i = 0; i < 4; ++i)
    {
        tf(lx[i], ly[i], q.x[i], q.y[i]);
        q.u[i] = us[i];
        q.v[i] = vs[i];
    }
    q.colour  = colour;
    q.texture = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tex));
    return true;
}

// D3DXMatrixTransformation2D(NULL, 0, scaling, rotation centre, rotation, translation): scale, rotate about the centre
// (in scaled space), translate.
int32_t spriteDraw(void* self, void* tex, const Rect* src, const Vec2* scale, const Vec2* centre, float rotation, const Vec2* translation,
                   uint32_t colour)
{
    auto*      s  = static_cast<SpriteImpl*>(asCom(self)->impl);
    const Vec2 sc = scale != nullptr ? *scale : Vec2{ 1, 1 };
    const Vec2 c  = centre != nullptr ? *centre : Vec2{ 0, 0 };
    const Vec2 tr = translation != nullptr ? *translation : Vec2{ 0, 0 };
    const float cs = std::cos(rotation), sn = std::sin(rotation);
    d3d::Quad  q;
    if (quadFor(tex, src, colour, q, [&](float x, float y, float& ox, float& oy) {
            const float px = x * sc.x - c.x, py = y * sc.y - c.y;
            ox             = px * cs - py * sn + c.x + tr.x;
            oy             = px * sn + py * cs + c.y + tr.y;
        }))
    {
        s->quads.push_back(q);
    }
    return kOk;
}
int32_t spriteDrawTransform(void* self, void* tex, const Rect* src, const Matrix* m, uint32_t colour)
{
    auto*     s = static_cast<SpriteImpl*>(asCom(self)->impl);
    Matrix    id{ { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } } };
    const Matrix& mm = m != nullptr ? *m : id;
    d3d::Quad q;
    if (quadFor(tex, src, colour, q, [&](float x, float y, float& ox, float& oy) {
            ox = x * mm.m[0][0] + y * mm.m[1][0] + mm.m[3][0];
            oy = x * mm.m[0][1] + y * mm.m[1][1] + mm.m[3][1];
        }))
    {
        s->quads.push_back(q);
    }
    return kOk;
}
int32_t spriteEnd(void* self)
{
    auto* s = static_cast<SpriteImpl*>(asCom(self)->impl);
    if (imguiFrame())
    {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (const auto& q : s->quads)
        {
            TextureImpl* t = texOf(q.texture);
            if (t == nullptr)
            {
                continue;
            }
            if (const auto ref = texRef(*t))
            {
                dl->AddImageQuad(*ref, { q.x[0], q.y[0] }, { q.x[1], q.y[1] }, { q.x[2], q.y[2] }, { q.x[3], q.y[3] }, { q.u[0], q.v[0] },
                                 { q.u[1], q.v[1] }, { q.u[2], q.v[2] }, { q.u[3], q.v[3] }, imColour(q.colour));
            }
        }
    }
    lastQuadsRef() = std::move(s->quads);
    s->quads.clear();
    s->begun = false;
    return kOk;
}

const std::array<Fn, 10>& spriteVtbl()
{
    static const std::array<Fn, 10> t = [] {
        auto v = stubTable<10>();
        v[3]   = reinterpret_cast<Fn>(&spriteGetDevice);
        v[4]   = reinterpret_cast<Fn>(&spriteBegin);
        v[5]   = reinterpret_cast<Fn>(&spriteDraw);
        v[6]   = reinterpret_cast<Fn>(&spriteDrawTransform);
        v[7]   = reinterpret_cast<Fn>(&spriteEnd);
        v[8]   = reinterpret_cast<Fn>(&texOk); // OnLostDevice
        v[9]   = reinterpret_cast<Fn>(&texOk); // OnResetDevice
        return v;
    }();
    return t;
}

// ---------------------------------------------------------------------------------------------------------------------
// D3DX functions (ffi.C stand-ins)
// ---------------------------------------------------------------------------------------------------------------------
int32_t createSprite(void*, void** out)
{
    if (out == nullptr)
    {
        return kInvalidCall;
    }
    Com* c = newCom(Kind::Sprite, spriteVtbl().data(), new SpriteImpl);
    *out   = c;
    return c != nullptr ? kOk : kFail;
}

// A Windows path to a file: '\' -> '/', and each part matched case-insensitively (addons' asset names do not always
// match their files' case).
std::filesystem::path resolvePath(const char* in)
{
    std::string s(in != nullptr ? in : "");
    std::replace(s.begin(), s.end(), '\\', '/');
    std::filesystem::path p(s);
    std::error_code       ec;
    if (std::filesystem::exists(p, ec))
    {
        return p;
    }
    std::filesystem::path cur = p.is_absolute() ? std::filesystem::path("/") : std::filesystem::path(".");
    for (const auto& part : p.relative_path())
    {
        std::filesystem::path next = cur / part;
        if (!std::filesystem::exists(next, ec))
        {
            const std::string want = part.string();
            bool              found = false;
            for (const auto& e : std::filesystem::directory_iterator(cur, ec))
            {
                const std::string name = e.path().filename().string();
                if (name.size() == want.size() &&
                    std::equal(name.begin(), name.end(), want.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
                {
                    next  = e.path();
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return p;
            }
        }
        cur = next;
    }
    return cur;
}

uint32_t fileFormat(const unsigned char* d, std::size_t n)
{
    if (n >= 2 && d[0] == 'B' && d[1] == 'M')
    {
        return 0; // D3DXIFF_BMP
    }
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8)
    {
        return 1; // JPG
    }
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P')
    {
        return 3; // PNG
    }
    return 2; // TGA
}

// Decodes an image file in memory into A8R8G8B8, resized to w x h when asked, colour key made transparent.
int32_t textureFromMemory(const void* data, std::size_t size, uint32_t w, uint32_t h, uint32_t colourKey, ImageInfo* info, void** out)
{
    if (out == nullptr || data == nullptr || size == 0)
    {
        return kInvalidCall;
    }
    *out    = nullptr;
    auto* d = static_cast<const unsigned char*>(data);
    if (size > 0x7FFFFFFF && size >= 6 && d[0] == 'B' && d[1] == 'M')
    {
        uint32_t bf = 0;
        std::memcpy(&bf, d + 2, 4); // BITMAPFILEHEADER.bfSize: the size an addon passing -1 means
        size = bf;
    }
    if (size > 0x7FFFFFFF)
    {
        return kInvalidData;
    }
    int  iw = 0, ih = 0, comp = 0;
    auto* px = stbi_load_from_memory(d, static_cast<int>(size), &iw, &ih, &comp, 4);
    if (px == nullptr)
    {
        return kInvalidData;
    }
    const int ow = (w == 0 || w >= 0xFFFFFFFEu) ? iw : static_cast<int>(w);
    const int oh = (h == 0 || h >= 0xFFFFFFFEu) ? ih : static_cast<int>(h);
    std::vector<uint32_t> argb(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh));
    for (int y = 0; y < oh; ++y)
    {
        for (int x = 0; x < ow; ++x)
        {
            // Nearest sample (sizes asked for in practice equal the file's).
            const int      sx = std::min(iw - 1, x * iw / ow), sy = std::min(ih - 1, y * ih / oh);
            const auto*    s  = px + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(iw) + static_cast<std::size_t>(sx)) * 4;
            uint32_t       c  = (static_cast<uint32_t>(s[3]) << 24) | (static_cast<uint32_t>(s[0]) << 16) | (static_cast<uint32_t>(s[1]) << 8) | s[2];
            if (colourKey != 0 && c == colourKey)
            {
                c = 0;
            }
            argb[static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x)] = c;
        }
    }
    stbi_image_free(px);
    if (info != nullptr)
    {
        *info = { static_cast<uint32_t>(iw), static_cast<uint32_t>(ih), 1, 1, kFmtA8R8G8B8, 3, fileFormat(d, size) };
    }
    Com* c = makeTexture(ow, oh, argb.data());
    *out   = c;
    return c != nullptr ? kOk : kFail;
}

std::vector<unsigned char> readFile(const char* path)
{
    std::ifstream f(resolvePath(path), std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), {});
}

int32_t textureFromFileEx(void*, const char* path, uint32_t w, uint32_t h, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                          uint32_t colourKey, ImageInfo* info, void*, void** out)
{
    const auto bytes = readFile(path);
    if (bytes.empty())
    {
        if (out != nullptr)
        {
            *out = nullptr;
        }
        spdlog::debug("ashita: d3d8: no image file {}", path != nullptr ? path : "(null)");
        return kInvalidData;
    }
    return textureFromMemory(bytes.data(), bytes.size(), w, h, colourKey, info, out);
}
int32_t textureFromFile(void* dev, const char* path, void** out)
{
    return textureFromFileEx(dev, path, kD3dxDefault, kD3dxDefault, 0, 0, 0, 0, 0, 0, 0, nullptr, nullptr, out);
}
int32_t textureFromMemoryEx(void*, const void* data, uint32_t size, uint32_t w, uint32_t h, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                            uint32_t, uint32_t colourKey, ImageInfo* info, void*, void** out)
{
    return textureFromMemory(data, size, w, h, colourKey, info, out);
}
int32_t textureFromMemoryPlain(void*, const void* data, uint32_t size, void** out)
{
    return textureFromMemory(data, size, kD3dxDefault, kD3dxDefault, 0, nullptr, out);
}

// ---------------------------------------------------------------------------------------------------------------------
// gdifonttexture.dll: GDI+ text and rounded rectangles drawn into textures (Thorny's gdifonts; ~/git/gdifonttexture's
// GdiFontManager.cpp has the semantics). Text here is stb_truetype on the font fontconfig picks for the family.
// ---------------------------------------------------------------------------------------------------------------------
struct FontFile
{
    std::vector<unsigned char> bytes;
    stbtt_fontinfo             info{};
    bool                       ok = false;
};

std::string shellSafe(const std::string& s)
{
    std::string out;
    for (const char c : s)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.')
        {
            out.push_back(c);
        }
    }
    return out;
}

std::string fcMatch(const std::string& pattern, const char* format)
{
    const std::string cmd = std::string("fc-match -f '") + format + "' '" + pattern + "' 2>/dev/null";
    std::string       out;
    if (FILE* p = popen(cmd.c_str(), "r"))
    {
        char buf[1024];
        std::size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0)
        {
            out.append(buf, n);
        }
        pclose(p);
    }
    return out;
}

// SetFontFolder (fancychat's gdifonttexture build): the font files of a folder, by family (name id 1, case folded) and
// style (head.macStyle bit 0 bold, bit 1 italic), searched before fontconfig.
struct PrivateFont
{
    std::string family;
    bool        bold = false, italic = false;
    std::unique_ptr<FontFile> font;
};
std::vector<PrivateFont>& privateFonts()
{
    static std::vector<PrivateFont> fonts;
    return fonts;
}

std::string lowerAscii(std::string s)
{
    for (auto& c : s)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

void addFontFolder(const std::filesystem::path& folder)
{
    static std::vector<std::filesystem::path> added; // each folder once (an addon reload sets it again)
    std::error_code                           ec;
    const auto                                key = std::filesystem::weakly_canonical(folder, ec);
    if (std::find(added.begin(), added.end(), key) != added.end())
    {
        return;
    }
    added.push_back(key);
    for (const auto& e : std::filesystem::directory_iterator(folder, ec))
    {
        const auto ext = lowerAscii(e.path().extension().string());
        if (ext != ".ttf" && ext != ".otf")
        {
            continue;
        }
        auto          f = std::make_unique<FontFile>();
        std::ifstream in(e.path(), std::ios::binary);
        f->bytes.assign(std::istreambuf_iterator<char>(in), {});
        if (f->bytes.empty() || stbtt_InitFont(&f->info, f->bytes.data(), stbtt_GetFontOffsetForIndex(f->bytes.data(), 0)) == 0)
        {
            continue;
        }
        f->ok   = true;
        int len = 0;
        const char* name = stbtt_GetFontNameString(&f->info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
        std::string family;
        for (int i = 1; name != nullptr && i < len; i += 2) // UTF-16BE: the low bytes
        {
            family += name[i];
        }
        int style = 0;
        if (const stbtt_uint32 head = stbtt__find_table(f->info.data, static_cast<stbtt_uint32>(f->info.fontstart), "head"); head != 0)
        {
            style = (f->info.data[head + 44] << 8) | f->info.data[head + 45];
        }
        if (family.empty())
        {
            continue;
        }
        spdlog::info("ashita: gdifonts: '{}'{}{} from {}", family, (style & 1) ? " bold" : "", (style & 2) ? " italic" : "", e.path().string());
        privateFonts().push_back({ lowerAscii(family), (style & 1) != 0, (style & 2) != 0, std::move(f) });
    }
}

const FontFile* privateFont(const std::string& family, bool bold, bool italic)
{
    const auto       want = lowerAscii(family);
    const FontFile*  any  = nullptr;
    for (const auto& p : privateFonts())
    {
        if (p.family != want)
        {
            continue;
        }
        if (p.bold == bold && p.italic == italic)
        {
            return p.font.get();
        }
        if (any == nullptr || (!p.bold && !p.italic))
        {
            any = p.font.get();
        }
    }
    return any;
}

const FontFile* loadFont(const std::string& family, bool bold, bool italic)
{
    if (const FontFile* p = privateFont(family, bold, italic))
    {
        return p;
    }
    static std::map<std::string, std::unique_ptr<FontFile>> cache;
    const std::string pattern = (shellSafe(family).empty() ? std::string("sans") : shellSafe(family)) + (bold ? ":bold" : "") + (italic ? ":italic" : "");
    if (const auto it = cache.find(pattern); it != cache.end())
    {
        return it->second->ok ? it->second.get() : nullptr;
    }
    auto       f    = std::make_unique<FontFile>();
    const auto spec = fcMatch(pattern, "%{file}|%{index}");
    const auto bar  = spec.find('|');
    if (bar != std::string::npos)
    {
        const std::string file  = spec.substr(0, bar);
        const int         index = std::atoi(spec.c_str() + bar + 1);
        std::ifstream     in(file, std::ios::binary);
        f->bytes.assign(std::istreambuf_iterator<char>(in), {});
        if (!f->bytes.empty())
        {
            const int off = stbtt_GetFontOffsetForIndex(f->bytes.data(), index);
            f->ok         = off >= 0 && stbtt_InitFont(&f->info, f->bytes.data(), off) != 0;
        }
        spdlog::info("ashita: gdifonts: '{}' -> {}{}", pattern, file, f->ok ? "" : " (unreadable)");
    }
    auto* raw = f.get();
    cache.emplace(pattern, std::move(f));
    return raw->ok ? raw : nullptr;
}

// GetFontAvailable: Windows answers for the families installed; here fontconfig substitutes the nearest face (Arial ->
// Liberation Sans) and that face is drawn, so any family that resolves to a font file counts as available. Addons then
// keep the family they asked for instead of falling back.
bool fontAvailable(const char* name)
{
    if (name == nullptr || *name == 0)
    {
        return false;
    }
    return loadFont(name, false, false) != nullptr;
}

std::vector<uint32_t> utf8(const char* s)
{
    std::vector<uint32_t> out;
    const auto*           p = reinterpret_cast<const unsigned char*>(s);
    while (*p != 0)
    {
        uint32_t c = *p++;
        int      n = 0;
        if (c >= 0xF0)
        {
            c &= 0x07;
            n = 3;
        }
        else if (c >= 0xE0)
        {
            c &= 0x0F;
            n = 2;
        }
        else if (c >= 0xC0)
        {
            c &= 0x1F;
            n = 1;
        }
        for (; n > 0 && (*p & 0xC0) == 0x80; --n)
        {
            c = (c << 6) | (*p++ & 0x3F);
        }
        out.push_back(c);
    }
    return out;
}

// RGBA in floats, premultiplied, for compositing.
struct Px
{
    float r = 0, g = 0, b = 0, a = 0;
};
Px premul(uint32_t argb, float cover)
{
    const float a = static_cast<float>(argb >> 24) / 255.0f * cover;
    return { static_cast<float>((argb >> 16) & 0xFF) / 255.0f * a, static_cast<float>((argb >> 8) & 0xFF) / 255.0f * a,
             static_cast<float>(argb & 0xFF) / 255.0f * a, a };
}
Px over(Px top, Px under)
{
    const float k = 1.0f - top.a;
    return { top.r + under.r * k, top.g + under.g * k, top.b + under.b * k, top.a + under.a * k };
}
uint32_t lerpColour(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
    {
        const float ca = static_cast<float>((a >> s) & 0xFF), cb = static_cast<float>((b >> s) & 0xFF);
        out |= static_cast<uint32_t>(std::lround(ca + (cb - ca) * t)) << s;
    }
    return out;
}

// The brush GdiFontManager::GetBrush makes: solid, or a linear gradient between two corners / edges of w x h.
uint32_t brushAt(uint32_t colour, uint32_t style, uint32_t gradient, float x, float y, float w, float h)
{
    if (style == 0 || style > 8)
    {
        return colour;
    }
    float sx = 0, sy = 0, ex = 0, ey = 0;
    switch (style)
    {
        case 1: ex = w; break;
        case 2: ex = w; ey = h; break;
        case 3: ey = h; break;
        case 4: sx = w; ey = h; break;
        case 5: sx = w; break;
        case 6: sx = w; sy = h; break;
        case 7: sy = h; break;
        case 8: sy = h; ex = w; break;
        default: break;
    }
    const float dx = ex - sx, dy = ey - sy, len2 = dx * dx + dy * dy;
    const float t  = len2 > 0 ? std::clamp(((x - sx) * dx + (y - sy) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    return lerpColour(colour, gradient, t);
}

// Grey-scale coverage; max-filter with a disc of radius r (dilation).
std::vector<float> dilate(const std::vector<float>& m, int w, int h, float r)
{
    if (r <= 0)
    {
        return m;
    }
    const int          ri = static_cast<int>(std::ceil(r));
    std::vector<float> out(m.size(), 0.0f);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float best = 0;
            for (int dy = -ri; dy <= ri && best < 1.0f; ++dy)
            {
                const int yy = y + dy;
                if (yy < 0 || yy >= h)
                {
                    continue;
                }
                for (int dx = -ri; dx <= ri; ++dx)
                {
                    const int xx = x + dx;
                    if (xx < 0 || xx >= w)
                    {
                        continue;
                    }
                    const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                    if (d > r + 0.5f)
                    {
                        continue;
                    }
                    const float edge = std::clamp(r + 0.5f - d, 0.0f, 1.0f); // soft rim
                    best             = std::max(best, m[static_cast<std::size_t>(yy * w + xx)] * edge);
                }
            }
            out[static_cast<std::size_t>(y * w + x)] = best;
        }
    }
    return out;
}

// Downsamples a premultiplied canvas by ss and writes non-premultiplied A8R8G8B8 of the rows / columns kept.
std::vector<uint32_t> resolve(const std::vector<Px>& c, int cw, int x0, int y0, int ow, int oh, int ss)
{
    std::vector<uint32_t> out(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh), 0u);
    const float           n = static_cast<float>(ss * ss);
    for (int y = 0; y < oh; ++y)
    {
        for (int x = 0; x < ow; ++x)
        {
            Px s;
            for (int sy = 0; sy < ss; ++sy)
            {
                for (int sx = 0; sx < ss; ++sx)
                {
                    const Px& p = c[static_cast<std::size_t>((y0 + y * ss + sy) * cw + x0 + x * ss + sx)];
                    s.r += p.r;
                    s.g += p.g;
                    s.b += p.b;
                    s.a += p.a;
                }
            }
            s.r /= n;
            s.g /= n;
            s.b /= n;
            s.a /= n;
            if (s.a <= 0.0f)
            {
                continue;
            }
            const auto ch = [&](float v) { return static_cast<uint32_t>(std::clamp(std::lround(v / s.a * 255.0f), 0L, 255L)); };
            out[static_cast<std::size_t>(y * ow + x)] = (static_cast<uint32_t>(std::lround(std::clamp(s.a, 0.0f, 1.0f) * 255.0f)) << 24) |
                                                       (ch(s.r) << 16) | (ch(s.g) << 8) | ch(s.b);
        }
    }
    return out;
}

// GdiFontManager::CreateFontTexture: the text laid out in BoxWidth (wrapping at spaces), outline stroked under the
// fill, trimmed to the columns used and from the layout's top to the last row used.
// `marks`: CreateTextureColor (fancychat's gdifonttexture build): "\\§AARRGGBBç\\" in the text starts a run in that
// colour, "\\§--------ç\\" goes back to FontColor; the marks are not drawn (INFERRED from fancychat's own use and its
// cleanMC, not from the DLL's code).
Com* fontTexture(const GdiFontData& d, int32_t& outW, int32_t& outH, bool marks = false)
{
    outW = outH = 0;
    constexpr int ss    = 2;
    const bool    bold  = (d.FontFlags & 1) != 0;
    const bool    ital  = (d.FontFlags & 2) != 0;
    const FontFile* f   = loadFont(std::string(d.FontFamily, strnlen(d.FontFamily, sizeof d.FontFamily)), bold, ital);
    const FontFile* fb  = loadFont("sans", bold, ital);
    if (f == nullptr)
    {
        f = fb;
    }
    if (f == nullptr || d.FontHeight <= 0.0f)
    {
        return nullptr;
    }
    auto                  text = utf8(std::string(d.FontText, strnlen(d.FontText, sizeof d.FontText)).c_str());
    std::vector<uint32_t> colours(text.size(), 0); // 0: FontColor (and its gradient)
    if (marks)
    {
        std::vector<uint32_t> plain, runs;
        uint32_t              run = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (i + 11 < text.size() && text[i] == '\\' && text[i + 1] == 0xA7 && text[i + 10] == 0xE7 && text[i + 11] == '\\')
            {
                uint32_t v  = 0;
                bool     ok = true;
                for (std::size_t k = i + 2; k < i + 10; ++k)
                {
                    const uint32_t c = text[k];
                    const int      h = c >= '0' && c <= '9' ? static_cast<int>(c - '0') : c >= 'a' && c <= 'f' ? static_cast<int>(c - 'a' + 10) : c >= 'A' && c <= 'F' ? static_cast<int>(c - 'A' + 10) : -1;
                    ok = ok && h >= 0;
                    v  = (v << 4) | static_cast<uint32_t>(std::max(h, 0));
                }
                run = ok ? v : 0;
                i += 11;
                continue;
            }
            plain.push_back(text[i]);
            runs.push_back(run);
        }
        text    = std::move(plain);
        colours = std::move(runs);
    }
    if (text.empty())
    {
        return nullptr;
    }
    const float em    = d.FontHeight * static_cast<float>(ss);
    const float scale = stbtt_ScaleForMappingEmToPixels(&f->info, em);
    int         asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    // GDI+ cell ascent / descent are the OS/2 table's usWinAscent / usWinDescent (stb_truetype's OS/2 call gives the
    // typographic ones).
    if (const stbtt_uint32 os2 = stbtt__find_table(f->info.data, static_cast<stbtt_uint32>(f->info.fontstart), "OS/2"); os2 != 0)
    {
        const unsigned char* t = f->info.data + os2;
        const int wa = (t[74] << 8) | t[75], wd = (t[76] << 8) | t[77];
        if (wa > 0)
        {
            asc  = wa;
            desc = -wd;
        }
    }
    const float ascent  = static_cast<float>(asc) * scale;
    const float lineH   = std::max(static_cast<float>(asc - desc + gap), static_cast<float>(asc - desc)) * scale;
    const float boxW    = static_cast<float>(d.BoxWidth > 0 ? d.BoxWidth : 1024) * static_cast<float>(ss);
    const float outline = ((d.OutlineColor >> 24) != 0) ? std::max(0.0f, d.OutlineWidth) * static_cast<float>(ss) : 0.0f;

    // Glyph placement: x advance with kerning, wrapping at the last space when a line passes the box.
    struct Glyph
    {
        const FontFile* font;
        int             glyph;
        float           x, baseline;
        uint32_t        colour = 0; // a colour mark's (0: FontColor)
    };
    std::vector<Glyph> glyphs;
    float              penX = 0, line = 0;
    std::size_t        lineStart = 0, lastSpace = std::string::npos;
    float              maxW = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const uint32_t cp = text[i];
        if (cp == '\n')
        {
            maxW = std::max(maxW, penX);
            penX = 0;
            line += 1;
            lineStart = glyphs.size();
            lastSpace = std::string::npos;
            continue;
        }
        const FontFile* ff = f;
        int             g  = stbtt_FindGlyphIndex(&ff->info, static_cast<int>(cp));
        if (g == 0 && fb != nullptr && fb != f)
        {
            if (const int g2 = stbtt_FindGlyphIndex(&fb->info, static_cast<int>(cp)); g2 != 0)
            {
                ff = fb;
                g  = g2;
            }
        }
        const float sc  = ff == f ? scale : stbtt_ScaleForMappingEmToPixels(&ff->info, em);
        int         adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&ff->info, g, &adv, &lsb);
        if (!glyphs.empty() && glyphs.back().font == ff && glyphs.size() > lineStart)
        {
            penX += static_cast<float>(stbtt_GetGlyphKernAdvance(&ff->info, glyphs.back().glyph, g)) * sc;
        }
        const float w = static_cast<float>(adv) * sc;
        if (penX + w > boxW && lastSpace != std::string::npos && lastSpace > lineStart && lastSpace < glyphs.size())
        {
            // Move the glyphs after the last space to a new line.
            const float shift = glyphs[lastSpace].x;
            maxW              = std::max(maxW, shift);
            line += 1;
            for (std::size_t k = lastSpace; k < glyphs.size(); ++k)
            {
                glyphs[k].x -= shift;
                glyphs[k].baseline = line * lineH + ascent;
            }
            penX -= shift;
            lineStart = lastSpace;
            lastSpace = std::string::npos;
        }
        if (cp == ' ')
        {
            lastSpace = glyphs.size() + 1;
        }
        glyphs.push_back({ ff, g, penX, line * lineH + ascent, colours[i] });
        penX += w;
    }
    maxW = std::max(maxW, penX);

    const int pad = static_cast<int>(std::ceil(outline)) + 4 * ss;
    const int cw  = static_cast<int>(std::ceil(maxW)) + 2 * pad;
    const int ch  = static_cast<int>(std::ceil((line + 1) * lineH)) + 2 * pad;
    std::vector<float>    mask(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch), 0.0f);
    std::vector<uint32_t> colourAt(marks ? mask.size() : 0, 0);
    for (const auto& gl : glyphs)
    {
        const float sc = gl.font == f ? scale : stbtt_ScaleForMappingEmToPixels(&gl.font->info, em);
        const float ox = static_cast<float>(pad) + gl.x, oy = static_cast<float>(pad) + gl.baseline;
        const float fx = ox - std::floor(ox), fy = oy - std::floor(oy);
        int         x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(&gl.font->info, gl.glyph, sc, sc, fx, fy, &x0, &y0, &x1, &y1);
        const int bw = x1 - x0, bh = y1 - y0;
        if (bw <= 0 || bh <= 0)
        {
            continue;
        }
        std::vector<unsigned char> bmp(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh));
        stbtt_MakeGlyphBitmapSubpixel(&gl.font->info, bmp.data(), bw, bh, bw, sc, sc, fx, fy, gl.glyph);
        const int bx = static_cast<int>(std::floor(ox)) + x0, by = static_cast<int>(std::floor(oy)) + y0;
        for (int y = 0; y < bh; ++y)
        {
            for (int x = 0; x < bw; ++x)
            {
                const int px = bx + x, py = by + y;
                if (px < 0 || py < 0 || px >= cw || py >= ch)
                {
                    continue;
                }
                float& m = mask[static_cast<std::size_t>(py * cw + px)];
                m        = std::min(1.0f, m + static_cast<float>(bmp[static_cast<std::size_t>(y * bw + x)]) / 255.0f);
                if (!colourAt.empty() && bmp[static_cast<std::size_t>(y * bw + x)] != 0)
                {
                    colourAt[static_cast<std::size_t>(py * cw + px)] = gl.colour;
                }
            }
        }
    }
    // Underline / strikeout (FontFlags 4 / 8) as bars across each line's glyphs.
    if ((d.FontFlags & 12) != 0)
    {
        const float thick = std::max(static_cast<float>(ss), em * 0.06f);
        for (int l = 0; l <= static_cast<int>(line); ++l)
        {
            float x0 = 1e9f, x1 = -1e9f;
            for (const auto& gl : glyphs)
            {
                if (std::lround((gl.baseline - ascent) / lineH) == l)
                {
                    x0 = std::min(x0, gl.x);
                    x1 = std::max(x1, gl.x + em * 0.5f);
                }
            }
            if (x1 <= x0)
            {
                continue;
            }
            const float base = static_cast<float>(l) * lineH + ascent + static_cast<float>(pad);
            for (const float yc : { (d.FontFlags & 4) != 0 ? base + em * 0.12f : -1.0f, (d.FontFlags & 8) != 0 ? base - em * 0.28f : -1.0f })
            {
                if (yc < 0)
                {
                    continue;
                }
                for (int y = static_cast<int>(yc); y < static_cast<int>(yc + thick) && y < ch; ++y)
                {
                    for (int x = static_cast<int>(x0) + pad; x < static_cast<int>(x1) + pad && x < cw; ++x)
                    {
                        mask[static_cast<std::size_t>(y * cw + x)] = 1.0f;
                    }
                }
            }
        }
    }

    // GDI+ strokes the path with a pen OutlineWidth wide (half outside, half inside), then fills it on top.
    std::vector<float> ring;
    if (outline > 0)
    {
        const auto         outer = dilate(mask, cw, ch, outline / 2);
        std::vector<float> inv(mask.size());
        for (std::size_t i = 0; i < mask.size(); ++i)
        {
            inv[i] = 1.0f - mask[i];
        }
        const auto innerHole = dilate(inv, cw, ch, outline / 2); // 1 - erode(mask)
        ring.resize(mask.size());
        for (std::size_t i = 0; i < mask.size(); ++i)
        {
            ring[i] = std::clamp(outer[i] * innerHole[i] + (outer[i] - mask[i]), 0.0f, 1.0f);
        }
    }
    const bool       fill = (d.FontColor >> 24) != 0 || (d.GradientStyle != 0 && (d.GradientColor >> 24) != 0);
    std::vector<Px>  canvas(mask.size());
    int              minX = cw, maxX = -1, maxY = -1;
    const float      gw = maxW, gh = (line + 1) * lineH;
    for (int y = 0; y < ch; ++y)
    {
        for (int x = 0; x < cw; ++x)
        {
            const std::size_t i = static_cast<std::size_t>(y * cw + x);
            Px                p;
            if (!ring.empty() && ring[i] > 0)
            {
                p = premul(d.OutlineColor, ring[i]);
            }
            if (fill && mask[i] > 0)
            {
                const uint32_t col = !colourAt.empty() && colourAt[i] != 0
                                         ? colourAt[i]
                                         : brushAt(d.FontColor, d.GradientStyle, d.GradientColor, static_cast<float>(x - pad), static_cast<float>(y - pad), gw, gh);
                p                  = over(premul(col, mask[i]), p);
            }
            canvas[i] = p;
            if (p.a > 0.002f)
            {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < minX || maxY < 0)
    {
        return nullptr;
    }
    // Rows from the layout's top (pad) down; columns trimmed both sides, as the DLL scans them.
    const int x0 = minX - (minX % ss);
    const int y0 = pad - (pad % ss);
    const int ow = (maxX - x0) / ss + 1, oh = (maxY - y0) / ss + 1;
    std::vector<Px> padded = canvas;
    const int       needW = x0 + ow * ss, needH = y0 + oh * ss;
    if (needW > cw || needH > ch)
    {
        return nullptr;
    }
    auto argb = resolve(padded, cw, x0, y0, ow, oh, ss);
    outW      = ow;
    outH      = oh;
    return makeTexture(ow, oh, argb.data());
}

// GdiFontManager::CreateRectTexture: a rounded rectangle (radius Diameter) filled, then stroked OutlineWidth wide.
Com* rectTexture(const GdiRectData& d, int32_t& outW, int32_t& outH)
{
    outW = outH = 0;
    if (d.Width <= 0 || d.Height <= 0 || d.Width > 8192 || d.Height > 8192)
    {
        return nullptr;
    }
    const int   ow = static_cast<int>(d.Width), oh = static_cast<int>(d.Height);
    const auto  wpen = static_cast<int>(d.OutlineWidth);
    float       rx = 0, ry = 0, rw = static_cast<float>(ow), rh = static_cast<float>(oh);
    if (wpen != 0)
    {
        int inset = wpen / 2, shrink = wpen;
        if ((wpen % 2) != 0)
        {
            inset += 1;
            shrink += 1;
        }
        rx = static_cast<float>(inset);
        ry = rx;
        rw = static_cast<float>(ow - shrink);
        rh = static_cast<float>(oh - shrink);
    }
    const float rad = std::min({ static_cast<float>(std::max(0, d.Diameter)), rw / 2, rh / 2 });
    const float cx = rx + rw / 2, cy = ry + rh / 2, bx = rw / 2, by = rh / 2;
    const auto  sdf = [&](float px, float py) {
        const float qx = std::abs(px - cx) - bx + rad, qy = std::abs(py - cy) - by + rad;
        return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - rad;
    };
    const bool fill    = (d.FillColor >> 24) != 0 || (d.GradientStyle != 0 && (d.GradientColor >> 24) != 0);
    const bool stroke  = wpen > 0 && (d.OutlineColor >> 24) != 0;
    constexpr int n    = 4;
    std::vector<uint32_t> argb(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh), 0u);
    for (int y = 0; y < oh; ++y)
    {
        for (int x = 0; x < ow; ++x)
        {
            float inF = 0, inS = 0;
            for (int sy = 0; sy < n; ++sy)
            {
                for (int sx = 0; sx < n; ++sx)
                {
                    const float px = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / n, py = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / n;
                    const float dd = sdf(px, py);
                    inF += dd <= 0 ? 1.0f : 0.0f;
                    inS += std::abs(dd) <= static_cast<float>(wpen) / 2 ? 1.0f : 0.0f;
                }
            }
            inF /= n * n;
            inS /= n * n;
            Px p;
            if (fill && inF > 0)
            {
                p = premul(brushAt(d.FillColor, d.GradientStyle, d.GradientColor, static_cast<float>(x), static_cast<float>(y), static_cast<float>(ow),
                                   static_cast<float>(oh)),
                           inF);
            }
            if (stroke && inS > 0)
            {
                p = over(premul(d.OutlineColor, inS), p);
            }
            if (p.a <= 0)
            {
                continue;
            }
            const auto ch = [&](float v) { return static_cast<uint32_t>(std::clamp(std::lround(v / p.a * 255.0f), 0L, 255L)); };
            argb[static_cast<std::size_t>(y * ow + x)] = (static_cast<uint32_t>(std::lround(std::clamp(p.a, 0.0f, 1.0f) * 255.0f)) << 24) |
                                                        (ch(p.r) << 16) | (ch(p.g) << 8) | ch(p.b);
        }
    }
    outW = ow;
    outH = oh;
    return makeTexture(ow, oh, argb.data());
}

void* gdiCreateManager(void*)
{
    return newCom(Kind::FontManager, stubTable<3>().data(), nullptr);
}
void gdiDestroyManager(void* m)
{
    if (pool().live(reinterpret_cast<uintptr_t>(m)) && asCom(m)->kind == Kind::FontManager)
    {
        pool().put(asCom(m));
    }
}
void* gdiCreateTexture(void*, const GdiFontData* d, int32_t* wh)
{
    int32_t w = 0, h = 0;
    Com*    c = d != nullptr ? fontTexture(*d, w, h) : nullptr;
    if (wh != nullptr)
    {
        wh[0] = w;
        wh[1] = h;
    }
    return c;
}
void* gdiCreateTextureColor(void*, const GdiFontData* d, int32_t* wh)
{
    int32_t w = 0, h = 0;
    Com*    c = d != nullptr ? fontTexture(*d, w, h, true) : nullptr;
    if (wh != nullptr)
    {
        wh[0] = w;
        wh[1] = h;
    }
    return c;
}
void gdiSetFontFolder(const char* folder)
{
    if (folder != nullptr && *folder != 0)
    {
        addFontFolder(std::filesystem::path(folder));
    }
}
void* gdiCreateRect(void*, const GdiRectData* d, int32_t* wh)
{
    int32_t w = 0, h = 0;
    Com*    c = d != nullptr ? rectTexture(*d, w, h) : nullptr;
    if (wh != nullptr)
    {
        wh[0] = w;
        wh[1] = h;
    }
    return c;
}
int32_t gdiFontAvailable(const char* name)
{
    return fontAvailable(name) ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// ImGui with D3D textures: imgui.Image / ImageWithBg / ImageButton and the draw lists' AddImage* take a texture as the
// number addons make of its pointer. prelude_d3d.lua normalises the arguments and calls these.
// ---------------------------------------------------------------------------------------------------------------------
float num(lua_State* L, int i, float def = 0.0f)
{
    return lua_isnumber(L, i) != 0 ? static_cast<float>(lua_tonumber(L, i)) : def;
}
uint64_t idArg(lua_State* L, int i)
{
    return lua_isnumber(L, i) != 0 ? static_cast<uint64_t>(lua_tonumber(L, i)) : 0;
}
std::optional<ImTextureRef> refArg(lua_State* L, int i)
{
    TextureImpl* t = texOf(idArg(L, i));
    return t != nullptr ? texRef(*t) : std::nullopt;
}
ImVec4 vec4At(lua_State* L, int i)
{
    return { num(L, i), num(L, i + 1), num(L, i + 2), num(L, i + 3) };
}

// image(id, w, h, u0, v0, u1, v1)
int lImage(lua_State* L)
{
    const ImVec2 size(num(L, 2), num(L, 3));
    if (const auto r = refArg(L, 1))
    {
        ImGui::Image(*r, size, { num(L, 4), num(L, 5) }, { num(L, 6, 1), num(L, 7, 1) });
    }
    else
    {
        ImGui::Dummy(size);
    }
    return 0;
}
// image_bg(id, w, h, u0, v0, u1, v1, bg r g b a, tint r g b a)
int lImageWithBg(lua_State* L)
{
    const ImVec2 size(num(L, 2), num(L, 3));
    if (const auto r = refArg(L, 1))
    {
        ImGui::ImageWithBg(*r, size, { num(L, 4), num(L, 5) }, { num(L, 6, 1), num(L, 7, 1) }, vec4At(L, 8), vec4At(L, 12));
    }
    else
    {
        ImGui::Dummy(size);
    }
    return 0;
}
// image_button(str_id, id, w, h, u0, v0, u1, v1, bg r g b a, tint r g b a) -> pressed
int lImageButton(lua_State* L)
{
    const char*  sid = luaL_optstring(L, 1, "##image");
    const ImVec2 size(num(L, 3), num(L, 4));
    bool         pressed = false;
    if (const auto r = refArg(L, 2))
    {
        pressed = ImGui::ImageButton(sid, *r, size, { num(L, 5), num(L, 6) }, { num(L, 7, 1), num(L, 8, 1) }, vec4At(L, 9), vec4At(L, 13));
    }
    else
    {
        pressed = ImGui::Button(sid, size);
    }
    lua_pushboolean(L, pressed ? 1 : 0);
    return 1;
}
ImDrawList* listArg(lua_State* L)
{
    return static_cast<ImDrawList*>(lua_touserdata(L, 1));
}
// dl_image(list, id, x0, y0, x1, y1, u0, v0, u1, v1, col)
int lDlImage(lua_State* L)
{
    ImDrawList* dl = listArg(L);
    if (const auto r = refArg(L, 2); r && dl != nullptr)
    {
        dl->AddImage(*r, { num(L, 3), num(L, 4) }, { num(L, 5), num(L, 6) }, { num(L, 7), num(L, 8) }, { num(L, 9, 1), num(L, 10, 1) },
                     static_cast<ImU32>(lua_tonumber(L, 11)));
    }
    return 0;
}
// dl_image_rounded(list, id, x0, y0, x1, y1, u0, v0, u1, v1, col, rounding, flags)
int lDlImageRounded(lua_State* L)
{
    ImDrawList* dl = listArg(L);
    if (const auto r = refArg(L, 2); r && dl != nullptr)
    {
        dl->AddImageRounded(*r, { num(L, 3), num(L, 4) }, { num(L, 5), num(L, 6) }, { num(L, 7), num(L, 8) }, { num(L, 9, 1), num(L, 10, 1) },
                            static_cast<ImU32>(lua_tonumber(L, 11)), num(L, 12), static_cast<ImDrawFlags>(lua_tointeger(L, 13)));
    }
    return 0;
}
// dl_image_quad(list, id, 4 points, 4 uvs, col)
int lDlImageQuad(lua_State* L)
{
    ImDrawList* dl = listArg(L);
    if (const auto r = refArg(L, 2); r && dl != nullptr)
    {
        dl->AddImageQuad(*r, { num(L, 3), num(L, 4) }, { num(L, 5), num(L, 6) }, { num(L, 7), num(L, 8) }, { num(L, 9), num(L, 10) },
                         { num(L, 11), num(L, 12) }, { num(L, 13), num(L, 14) }, { num(L, 15), num(L, 16) }, { num(L, 17), num(L, 18) },
                         static_cast<ImU32>(lua_tonumber(L, 19)));
    }
    return 0;
}
// texture_size(id) -> w, h (or nothing)
int lTextureSize(lua_State* L)
{
    TextureImpl* t = texOf(idArg(L, 1));
    if (t == nullptr)
    {
        return 0;
    }
    lua_pushinteger(L, t->w);
    lua_pushinteger(L, t->h);
    return 2;
}
int lSweep(lua_State*)
{
    d3d::sweep();
    return 0;
}

} // namespace

namespace d3d
{

uint64_t createTexture(int w, int h, const uint32_t* argb)
{
    if (w <= 0 || h <= 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(makeTexture(w, h, argb)));
}
bool isTexture(uint64_t id)
{
    return texOf(id) != nullptr;
}
bool textureSize(uint64_t id, int& w, int& h)
{
    const TextureImpl* t = texOf(id);
    if (t == nullptr)
    {
        return false;
    }
    w = t->w;
    h = t->h;
    return true;
}
const uint32_t* texturePixels(uint64_t id)
{
    const TextureImpl* t = texOf(id);
    return t != nullptr ? t->argb.data() : nullptr;
}
void release(uint64_t id)
{
    if (pool().live(id))
    {
        xi::ashita::release(reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
    }
}
std::size_t liveTextures()
{
    return pool().count(Kind::Texture);
}
void* device()
{
    return deviceCom();
}
const std::vector<Quad>& lastQuads()
{
    return lastQuadsRef();
}
void sweep()
{
    auto& v = pendingDestroy();
    if (v.empty() || ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }
    std::erase_if(v, [](ImTextureData* t) {
        if (t->Status != ImTextureStatus_Destroyed)
        {
            return false;
        }
        ImGui::UnregisterUserTexture(t);
        IM_DELETE(t);
        return true;
    });
}

} // namespace d3d

void openD3d(lua_State* L)
{
    lua_newtable(L);
    lua_pushlightuserdata(L, deviceCom());
    lua_setfield(L, -2, "device");
    const std::pair<const char*, Fn> fns[] = {
        { "create_sprite", reinterpret_cast<Fn>(&createSprite) },
        { "texture_from_file", reinterpret_cast<Fn>(&textureFromFile) },
        { "texture_from_file_ex", reinterpret_cast<Fn>(&textureFromFileEx) },
        { "texture_from_memory", reinterpret_cast<Fn>(&textureFromMemoryPlain) },
        { "texture_from_memory_ex", reinterpret_cast<Fn>(&textureFromMemoryEx) },
        { "gdi_create_manager", reinterpret_cast<Fn>(&gdiCreateManager) },
        { "gdi_destroy_manager", reinterpret_cast<Fn>(&gdiDestroyManager) },
        { "gdi_create_texture", reinterpret_cast<Fn>(&gdiCreateTexture) },
        { "gdi_create_texture_color", reinterpret_cast<Fn>(&gdiCreateTextureColor) },
        { "gdi_set_font_folder", reinterpret_cast<Fn>(&gdiSetFontFolder) },
        { "gdi_create_rect", reinterpret_cast<Fn>(&gdiCreateRect) },
        { "gdi_font_available", reinterpret_cast<Fn>(&gdiFontAvailable) },
    };
    lua_newtable(L);
    for (const auto& [name, fn] : fns)
    {
        lua_pushlightuserdata(L, reinterpret_cast<void*>(fn));
        lua_setfield(L, -2, name);
    }
    lua_setfield(L, -2, "fn");
    const luaL_Reg reg[] = {
        { "image", lImage },
        { "image_bg", lImageWithBg },
        { "image_button", lImageButton },
        { "dl_image", lDlImage },
        { "dl_image_rounded", lDlImageRounded },
        { "dl_image_quad", lDlImageQuad },
        { "texture_size", lTextureSize },
        { "sweep", lSweep },
        { nullptr, nullptr },
    };
    for (const auto* r = reg; r->name != nullptr; ++r)
    {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_setglobal(L, "__xi_d3d");
}

} // namespace xi::ashita

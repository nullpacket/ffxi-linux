// SPDX-License-Identifier: GPL-3.0-or-later
// Direct3D 8 / D3DX 8 / gdifonttexture.dll stand-ins for Ashita addons (PLANS goal 31, docs/ashita-compat.md
// "Direct3D 8 and gdifonts").
//
// Addons draw with the COM objects Ashita's libs/d3d8 declares: they call methods through `lpVtbl` from LuaJIT, so the
// objects here are real C structures with vtables of C functions. Addons also hand a texture to ImGui as
// `tonumber(ffi.cast('uint32_t', texture))`, so every object lives below 4 GiB (mmap MAP_32BIT) and that number is the
// object's address. Textures become ImGui 1.92 user textures; ID3DXSprite draws go on ImGui's foreground draw list at
// End(), in the order they were made.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct lua_State;

namespace xi::ashita
{

void openD3d(lua_State* L); // the __xi_d3d table prelude_d3d.lua builds the D3D8 / D3DX / gdifonts stand-ins from

namespace d3d
{

// A texture's id is its COM pointer's address (below 4 GiB); 0 is none.
[[nodiscard]] uint64_t createTexture(int w, int h, const uint32_t* argb); // A8R8G8B8 pixels (nullptr: cleared); refs 1
[[nodiscard]] bool     isTexture(uint64_t id);
[[nodiscard]] bool     textureSize(uint64_t id, int& w, int& h);
[[nodiscard]] const uint32_t* texturePixels(uint64_t id); // A8R8G8B8, w * h
void                   release(uint64_t id);            // IUnknown::Release on any object made here
[[nodiscard]] std::size_t liveTextures();
[[nodiscard]] void*       device();                     // the one IDirect3DDevice8

// One ID3DXSprite quad as End() put it on the draw list: corners clockwise from the source rectangle's top left.
struct Quad
{
    float    x[4]{}, y[4]{}, u[4]{}, v[4]{};
    uint32_t colour  = 0; // D3DCOLOR (A8R8G8B8)
    uint64_t texture = 0;
};
[[nodiscard]] const std::vector<Quad>& lastQuads(); // the most recent End()'s quads (tests)

// Frees the GPU side of released textures once ImGui's backend has let them go; called once a frame.
void sweep();

} // namespace d3d

} // namespace xi::ashita

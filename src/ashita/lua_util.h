// SPDX-License-Identifier: GPL-3.0-or-later
// LuaJIT headers and the pieces the Ashita layer's source files share.
#pragma once

#include <lua.hpp>

namespace xi::ashita
{

class AddonManager;

void openImgui(lua_State* L);
void imguiFrameStart(lua_State* L); // lua_imgui.cpp: a new frame for this addon's Begin / End counts
void imguiClose(lua_State* L);      // the state is closing                               // lua_imgui.cpp: the __xi_imgui table
void openNative(lua_State* L, AddonManager* m, void* addon); // lua_native.cpp: __xi, struct
void openState(lua_State* L);                               // lua_state.cpp: game state / data into the table on top
void openD3d(lua_State* L);                                 // d3d8_emu.cpp: the __xi_d3d table (D3D8 / D3DX / gdifonts)
AddonManager& managerOf(lua_State* L);                      // the manager that owns this state
void loadPendingFonts();                                    // lua_native.cpp: faces font objects asked for
extern const char* const kPrelude;                          // prelude.lua (generated: prelude_lua.cpp)

} // namespace xi::ashita

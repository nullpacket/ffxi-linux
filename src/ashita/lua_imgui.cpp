// SPDX-License-Identifier: GPL-3.0-or-later
// Ashita's IGuiManager for Lua (libs/imgui.lua sets it as the imgui table's metatable index): Dear ImGui calls with
// Ashita's conventions. Values an item edits come in a table whose [1] holds them ({ false }, { 0.5 }, { 'text' });
// vectors are { x, y } / { r, g, b, a } tables; functions returning an ImVec2 return two numbers. Functions not bound
// here answer through the prelude's stub (it logs the name once and returns nothing).
#include "ashita/lua_util.h"

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace xi::ashita
{
namespace
{

ImVec2 vec2(lua_State* L, int i, ImVec2 def = { 0, 0 })
{
    if (!lua_istable(L, i))
    {
        return def;
    }
    lua_rawgeti(L, i, 1);
    lua_rawgeti(L, i, 2);
    const ImVec2 v{ static_cast<float>(luaL_optnumber(L, -2, def.x)), static_cast<float>(luaL_optnumber(L, -1, def.y)) };
    lua_pop(L, 2);
    return v;
}

ImVec4 vec4(lua_State* L, int i, ImVec4 def = { 1, 1, 1, 1 })
{
    if (lua_isnumber(L, i))
    {
        return ImGui::ColorConvertU32ToFloat4(static_cast<ImU32>(lua_tonumber(L, i)));
    }
    if (!lua_istable(L, i))
    {
        return def;
    }
    float v[4] = { def.x, def.y, def.z, def.w };
    for (int k = 0; k < 4; ++k)
    {
        lua_rawgeti(L, i, k + 1);
        if (lua_isnumber(L, -1))
        {
            v[k] = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
    }
    return { v[0], v[1], v[2], v[3] };
}

void pushVec2(lua_State* L, ImVec2 v)
{
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
}

int optInt(lua_State* L, int i, int def = 0)
{
    return lua_isnumber(L, i) ? static_cast<int>(lua_tointeger(L, i)) : def;
}
float optFloat(lua_State* L, int i, float def = 0)
{
    return lua_isnumber(L, i) ? static_cast<float>(lua_tonumber(L, i)) : def;
}
const char* optStr(lua_State* L, int i, const char* def = nullptr)
{
    return lua_isstring(L, i) ? lua_tostring(L, i) : def;
}

// A { value } reference: read [1], write it back.
template <typename T> T refGet(lua_State* L, int i, T def);
template <> bool refGet<bool>(lua_State* L, int i, bool def)
{
    if (!lua_istable(L, i))
    {
        return def;
    }
    lua_rawgeti(L, i, 1);
    const bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}
template <> float refGet<float>(lua_State* L, int i, float def)
{
    if (!lua_istable(L, i))
    {
        return def;
    }
    lua_rawgeti(L, i, 1);
    const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}
template <> int refGet<int>(lua_State* L, int i, int def)
{
    if (!lua_istable(L, i))
    {
        return def;
    }
    lua_rawgeti(L, i, 1);
    const int v = lua_isnumber(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : def;
    lua_pop(L, 1);
    return v;
}
void refSetBool(lua_State* L, int i, bool v)
{
    if (lua_istable(L, i))
    {
        lua_pushboolean(L, v);
        lua_rawseti(L, i, 1);
    }
}
void refSetNum(lua_State* L, int i, double v)
{
    if (lua_istable(L, i))
    {
        lua_pushnumber(L, v);
        lua_rawseti(L, i, 1);
    }
}

// Open Begin / BeginChild counts per addon state: an End with none open is ignored, as Ashita does.
std::unordered_map<lua_State*, std::pair<int, int>>& depth()
{
    static std::unordered_map<lua_State*, std::pair<int, int>> d;
    return d;
}

int Begin(lua_State* L)
{
    ++depth()[L].first;
    const char* name = luaL_checkstring(L, 1);
    const int   flags = optInt(L, 3);
    bool        open  = refGet<bool>(L, 2, true);
    const bool  r     = ImGui::Begin(name, lua_istable(L, 2) ? &open : nullptr, flags);
    refSetBool(L, 2, open);
    lua_pushboolean(L, r);
    return 1;
}
int End(lua_State* L)
{
    auto& d = depth()[L].first;
    if (d > 0)
    {
        --d;
        ImGui::End();
    }
    return 0;
}
int BeginChild(lua_State* L)
{
    ++depth()[L].second;
    const ImVec2 size = vec2(L, 2);
    bool         r    = false;
    if (lua_isnumber(L, 1))
    {
        r = ImGui::BeginChild(static_cast<ImGuiID>(lua_tonumber(L, 1)), size, optInt(L, 3), optInt(L, 4));
    }
    else
    {
        // Ashita v3-style (str_id, size, border bool, flags) also appears in addons.
        const int childFlags = lua_isboolean(L, 3) ? (lua_toboolean(L, 3) ? ImGuiChildFlags_Borders : 0) : optInt(L, 3);
        r                    = ImGui::BeginChild(luaL_checkstring(L, 1), size, childFlags, optInt(L, 4));
    }
    lua_pushboolean(L, r);
    return 1;
}
int EndChild(lua_State* L)
{
    auto& d = depth()[L].second;
    if (d > 0)
    {
        --d;
        ImGui::EndChild();
    }
    return 0;
}

#define XI_BOOL0(NAME)                                                                                                 \
    int NAME(lua_State* L)                                                                                             \
    {                                                                                                                  \
        lua_pushboolean(L, ImGui::NAME());                                                                             \
        return 1;                                                                                                      \
    }
#define XI_VOID0(NAME)                                                                                                 \
    int NAME(lua_State*)                                                                                               \
    {                                                                                                                  \
        ImGui::NAME();                                                                                                 \
        return 0;                                                                                                      \
    }
#define XI_NUM0(NAME)                                                                                                  \
    int NAME(lua_State* L)                                                                                             \
    {                                                                                                                  \
        lua_pushnumber(L, static_cast<double>(ImGui::NAME()));                                                         \
        return 1;                                                                                                      \
    }
#define XI_VEC0(NAME)                                                                                                  \
    int NAME(lua_State* L)                                                                                             \
    {                                                                                                                  \
        pushVec2(L, ImGui::NAME());                                                                                    \
        return 2;                                                                                                      \
    }

XI_BOOL0(IsWindowAppearing)
XI_BOOL0(IsWindowCollapsed)
XI_VEC0(GetWindowPos)
XI_VEC0(GetWindowSize)
XI_NUM0(GetWindowWidth)
XI_NUM0(GetWindowHeight)
XI_NUM0(GetScrollX)
XI_NUM0(GetScrollY)
XI_NUM0(GetScrollMaxX)
XI_NUM0(GetScrollMaxY)
XI_NUM0(GetFontSize)
XI_VOID0(PopFont)
XI_VOID0(PopItemWidth)
XI_NUM0(CalcItemWidth)
XI_VOID0(PopTextWrapPos)
XI_VEC0(GetCursorScreenPos)
XI_VEC0(GetContentRegionAvail)
XI_VEC0(GetCursorPos)
XI_NUM0(GetCursorPosX)
XI_NUM0(GetCursorPosY)
XI_VEC0(GetCursorStartPos)
XI_VOID0(Separator)
XI_VOID0(NewLine)
XI_VOID0(Spacing)
XI_VOID0(BeginGroup)
XI_VOID0(EndGroup)
XI_VOID0(AlignTextToFramePadding)
XI_NUM0(GetTextLineHeight)
XI_NUM0(GetTextLineHeightWithSpacing)
XI_NUM0(GetFrameHeight)
XI_NUM0(GetFrameHeightWithSpacing)
XI_VOID0(PopID)
XI_VOID0(Bullet)
XI_VOID0(EndCombo)
XI_VOID0(TreePop)
XI_NUM0(GetTreeNodeToLabelSpacing)
XI_VOID0(EndListBox)
XI_BOOL0(BeginMenuBar)
XI_VOID0(EndMenuBar)
XI_BOOL0(BeginMainMenuBar)
XI_VOID0(EndMainMenuBar)
XI_VOID0(EndMenu)
XI_BOOL0(BeginTooltip)
XI_VOID0(EndTooltip)
XI_BOOL0(BeginItemTooltip)
XI_VOID0(EndPopup)
XI_VOID0(CloseCurrentPopup)
XI_VOID0(EndTable)
XI_BOOL0(TableNextColumn)
XI_VOID0(TableHeadersRow)
XI_NUM0(TableGetColumnCount)
XI_NUM0(TableGetColumnIndex)
XI_NUM0(TableGetRowIndex)
XI_VOID0(NextColumn)
XI_NUM0(GetColumnIndex)
XI_NUM0(GetColumnsCount)
XI_VOID0(EndTabBar)
XI_VOID0(EndTabItem)
XI_VOID0(EndDisabled)
XI_VOID0(PopClipRect)
XI_VOID0(SetItemDefaultFocus)
XI_BOOL0(IsItemActive)
XI_BOOL0(IsItemFocused)
XI_BOOL0(IsItemVisible)
XI_BOOL0(IsItemEdited)
XI_BOOL0(IsItemActivated)
XI_BOOL0(IsItemDeactivated)
XI_BOOL0(IsItemDeactivatedAfterEdit)
XI_BOOL0(IsItemToggledOpen)
XI_BOOL0(IsAnyItemHovered)
XI_BOOL0(IsAnyItemActive)
XI_BOOL0(IsAnyItemFocused)
XI_VEC0(GetItemRectMin)
XI_VEC0(GetItemRectMax)
XI_VEC0(GetItemRectSize)
XI_NUM0(GetTime)
XI_NUM0(GetFrameCount)
XI_BOOL0(IsAnyMouseDown)
XI_VEC0(GetMousePos)
XI_VOID0(PopItemFlag)
XI_VOID0(SetNextWindowFocus)
XI_VOID0(EndDragDropSource)
XI_VOID0(EndDragDropTarget)
XI_BOOL0(BeginDragDropTarget)

int IsWindowFocused(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsWindowFocused(optInt(L, 1)));
    return 1;
}
int IsWindowHovered(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsWindowHovered(optInt(L, 1)));
    return 1;
}
int SetNextWindowPos(lua_State* L)
{
    ImGui::SetNextWindowPos(vec2(L, 1), optInt(L, 2), vec2(L, 3));
    return 0;
}
int SetNextWindowSize(lua_State* L)
{
    ImGui::SetNextWindowSize(vec2(L, 1), optInt(L, 2));
    return 0;
}
int SetNextWindowSizeConstraints(lua_State* L)
{
    ImGui::SetNextWindowSizeConstraints(vec2(L, 1), vec2(L, 2, { FLT_MAX, FLT_MAX }));
    return 0;
}
int SetNextWindowContentSize(lua_State* L)
{
    ImGui::SetNextWindowContentSize(vec2(L, 1));
    return 0;
}
int SetNextWindowCollapsed(lua_State* L)
{
    ImGui::SetNextWindowCollapsed(lua_toboolean(L, 1) != 0, optInt(L, 2));
    return 0;
}
int SetNextWindowBgAlpha(lua_State* L)
{
    ImGui::SetNextWindowBgAlpha(optFloat(L, 1, 1));
    return 0;
}
int SetWindowPos(lua_State* L)
{
    if (lua_isstring(L, 1))
    {
        ImGui::SetWindowPos(lua_tostring(L, 1), vec2(L, 2), optInt(L, 3));
    }
    else
    {
        ImGui::SetWindowPos(vec2(L, 1), optInt(L, 2));
    }
    return 0;
}
int SetWindowSize(lua_State* L)
{
    if (lua_isstring(L, 1))
    {
        ImGui::SetWindowSize(lua_tostring(L, 1), vec2(L, 2), optInt(L, 3));
    }
    else
    {
        ImGui::SetWindowSize(vec2(L, 1), optInt(L, 2));
    }
    return 0;
}
int SetWindowFontScale(lua_State* L)
{
    ImGui::SetWindowFontScale(optFloat(L, 1, 1));
    return 0;
}
int SetWindowFocus(lua_State* L)
{
    lua_isstring(L, 1) ? ImGui::SetWindowFocus(lua_tostring(L, 1)) : ImGui::SetWindowFocus();
    return 0;
}
int SetScrollX(lua_State* L)
{
    ImGui::SetScrollX(optFloat(L, 1));
    return 0;
}
int SetScrollY(lua_State* L)
{
    ImGui::SetScrollY(optFloat(L, 1));
    return 0;
}
int SetScrollHereX(lua_State* L)
{
    ImGui::SetScrollHereX(optFloat(L, 1, 0.5f));
    return 0;
}
int SetScrollHereY(lua_State* L)
{
    ImGui::SetScrollHereY(optFloat(L, 1, 0.5f));
    return 0;
}
int PushFont(lua_State* L)
{
    // A font from AddFontFromFileTTF (light userdata), or nil for the current one; ImGui 1.92 draws any font at any size.
    auto* font = lua_islightuserdata(L, 1) ? static_cast<ImFont*>(lua_touserdata(L, 1)) : nullptr;
    ImGui::PushFont(font, optFloat(L, 2, 0));
    return 0;
}
int GetFont(lua_State* L)
{
    lua_pushlightuserdata(L, ImGui::GetFont());
    return 1;
}
// AddFontFromFileTTF(path, size): Ashita's addition. Windows font paths (C:\\Windows\\Fonts\\verdanab.ttf) become the
// closest installed face through fontconfig; a path that exists here is used as it is.
std::string fontFileFor(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
    {
        return path;
    }
    std::string stem = std::filesystem::path(path).stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    static const std::unordered_map<std::string, std::string> kWindows = {
        { "verdana", "Verdana" },       { "verdanab", "Verdana:bold" },  { "tahoma", "Tahoma" },       { "tahomabd", "Tahoma:bold" },
        { "arial", "Arial" },           { "arialbd", "Arial:bold" },     { "segoeui", "Segoe UI" },    { "segoeuib", "Segoe UI:bold" },
        { "consola", "Consolas" },      { "consolab", "Consolas:bold" }, { "calibri", "Calibri" },     { "calibrib", "Calibri:bold" },
        { "times", "Times New Roman" }, { "cour", "Courier New" },       { "msgothic", "MS Gothic" }, { "meiryo", "Meiryo" },
    };
    const auto        it  = kWindows.find(stem);
    std::string       pat = it != kWindows.end() ? it->second : stem;
    std::string       clean;
    for (const char c : pat)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == ':' || c == '-')
        {
            clean.push_back(c);
        }
    }
    std::string file;
    if (FILE* p = popen(("fc-match -f '%{file}' '" + clean + "' 2>/dev/null").c_str(), "r"))
    {
        char       buf[1024] = {};
        const auto n         = fread(buf, 1, sizeof buf - 1, p);
        pclose(p);
        file.assign(buf, n);
    }
    return file;
}
int AddFontFromFileTTF(lua_State* L)
{
    static std::unordered_map<std::string, ImFont*> loaded;
    const std::string file = fontFileFor(luaL_checkstring(L, 1));
    if (file.empty())
    {
        lua_pushnil(L);
        return 1;
    }
    auto& f = loaded[file];
    if (!f)
    {
        ImFontConfig cfg;
        cfg.MergeMode = false;
        f             = ImGui::GetIO().Fonts->AddFontFromFileTTF(file.c_str(), optFloat(L, 2, 0), &cfg);
    }
    if (!f)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlightuserdata(L, f);
    return 1;
}
int PushStyleColor(lua_State* L)
{
    ImGui::PushStyleColor(optInt(L, 1), vec4(L, 2));
    return 0;
}
int PopStyleColor(lua_State* L)
{
    ImGui::PopStyleColor(optInt(L, 1, 1));
    return 0;
}
int PushStyleVar(lua_State* L)
{
    if (lua_istable(L, 2))
    {
        ImGui::PushStyleVar(optInt(L, 1), vec2(L, 2));
    }
    else
    {
        ImGui::PushStyleVar(optInt(L, 1), optFloat(L, 2));
    }
    return 0;
}
int PushStyleVarX(lua_State* L)
{
    ImGui::PushStyleVarX(optInt(L, 1), optFloat(L, 2));
    return 0;
}
int PushStyleVarY(lua_State* L)
{
    ImGui::PushStyleVarY(optInt(L, 1), optFloat(L, 2));
    return 0;
}
int PopStyleVar(lua_State* L)
{
    ImGui::PopStyleVar(optInt(L, 1, 1));
    return 0;
}
int PushItemFlag(lua_State* L)
{
    ImGui::PushItemFlag(optInt(L, 1), lua_toboolean(L, 2) != 0);
    return 0;
}
int PushItemWidth(lua_State* L)
{
    ImGui::PushItemWidth(optFloat(L, 1));
    return 0;
}
int SetNextItemWidth(lua_State* L)
{
    ImGui::SetNextItemWidth(optFloat(L, 1));
    return 0;
}
int PushTextWrapPos(lua_State* L)
{
    ImGui::PushTextWrapPos(optFloat(L, 1));
    return 0;
}
int SetCursorScreenPos(lua_State* L)
{
    ImGui::SetCursorScreenPos(vec2(L, 1));
    return 0;
}
int SetCursorPos(lua_State* L)
{
    ImGui::SetCursorPos(vec2(L, 1));
    return 0;
}
int SetCursorPosX(lua_State* L)
{
    ImGui::SetCursorPosX(optFloat(L, 1));
    return 0;
}
int SetCursorPosY(lua_State* L)
{
    ImGui::SetCursorPosY(optFloat(L, 1));
    return 0;
}
int SameLine(lua_State* L)
{
    ImGui::SameLine(optFloat(L, 1), optFloat(L, 2, -1));
    return 0;
}
int Dummy(lua_State* L)
{
    ImGui::Dummy(vec2(L, 1));
    return 0;
}
int Indent(lua_State* L)
{
    ImGui::Indent(optFloat(L, 1));
    return 0;
}
int Unindent(lua_State* L)
{
    ImGui::Unindent(optFloat(L, 1));
    return 0;
}
int PushID(lua_State* L)
{
    lua_isnumber(L, 1) ? ImGui::PushID(static_cast<int>(lua_tointeger(L, 1))) : ImGui::PushID(luaL_checkstring(L, 1));
    return 0;
}
int GetID(lua_State* L)
{
    lua_pushnumber(L, lua_isnumber(L, 1) ? ImGui::GetID(static_cast<int>(lua_tointeger(L, 1))) : ImGui::GetID(luaL_checkstring(L, 1)));
    return 1;
}
int Text(lua_State* L)
{
    ImGui::TextUnformatted(luaL_optstring(L, 1, ""));
    return 0;
}
int TextColored(lua_State* L)
{
    ImGui::PushStyleColor(ImGuiCol_Text, vec4(L, 1));
    ImGui::TextUnformatted(luaL_optstring(L, 2, ""));
    ImGui::PopStyleColor();
    return 0;
}
int TextDisabled(lua_State* L)
{
    ImGui::TextDisabled("%s", luaL_optstring(L, 1, ""));
    return 0;
}
int TextWrapped(lua_State* L)
{
    ImGui::TextWrapped("%s", luaL_optstring(L, 1, ""));
    return 0;
}
int LabelText(lua_State* L)
{
    ImGui::LabelText(luaL_checkstring(L, 1), "%s", luaL_optstring(L, 2, ""));
    return 0;
}
int BulletText(lua_State* L)
{
    ImGui::BulletText("%s", luaL_optstring(L, 1, ""));
    return 0;
}
int SeparatorText(lua_State* L)
{
    ImGui::SeparatorText(luaL_optstring(L, 1, ""));
    return 0;
}
int Button(lua_State* L)
{
    lua_pushboolean(L, ImGui::Button(luaL_checkstring(L, 1), vec2(L, 2)));
    return 1;
}
int SmallButton(lua_State* L)
{
    lua_pushboolean(L, ImGui::SmallButton(luaL_checkstring(L, 1)));
    return 1;
}
int InvisibleButton(lua_State* L)
{
    lua_pushboolean(L, ImGui::InvisibleButton(luaL_checkstring(L, 1), vec2(L, 2, { 1, 1 }), optInt(L, 3)));
    return 1;
}
int ArrowButton(lua_State* L)
{
    lua_pushboolean(L, ImGui::ArrowButton(luaL_checkstring(L, 1), static_cast<ImGuiDir>(optInt(L, 2))));
    return 1;
}
int Checkbox(lua_State* L)
{
    bool       v = refGet<bool>(L, 2, false);
    const bool r = ImGui::Checkbox(luaL_checkstring(L, 1), &v);
    refSetBool(L, 2, v);
    lua_pushboolean(L, r);
    return 1;
}
int RadioButton(lua_State* L)
{
    if (lua_istable(L, 2))
    {
        int        v = refGet<int>(L, 2, 0);
        const bool r = ImGui::RadioButton(luaL_checkstring(L, 1), &v, optInt(L, 3));
        refSetNum(L, 2, v);
        lua_pushboolean(L, r);
    }
    else
    {
        lua_pushboolean(L, ImGui::RadioButton(luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0));
    }
    return 1;
}
int ProgressBar(lua_State* L)
{
    ImGui::ProgressBar(optFloat(L, 1), vec2(L, 2, { -FLT_MIN, 0 }), optStr(L, 3));
    return 0;
}
int BeginCombo(lua_State* L)
{
    lua_pushboolean(L, ImGui::BeginCombo(luaL_checkstring(L, 1), luaL_optstring(L, 2, ""), optInt(L, 3)));
    return 1;
}
int Combo(lua_State* L)
{
    int                      cur = refGet<int>(L, 2, 0);
    std::vector<std::string> items;
    if (lua_istable(L, 3))
    {
        const int n = optInt(L, 4, static_cast<int>(lua_objlen(L, 3)));
        for (int k = 1; k <= n; ++k)
        {
            lua_rawgeti(L, 3, k);
            items.emplace_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
            lua_pop(L, 1);
        }
    }
    else if (lua_isstring(L, 3))
    {
        std::size_t len = 0;
        const char* s   = lua_tolstring(L, 3, &len);
        for (std::size_t i = 0; i < len;)
        {
            const std::size_t e = std::find(s + i, s + len, '\0') - s;
            if (e == i)
            {
                break;
            }
            items.emplace_back(s + i, e - i);
            i = e + 1;
        }
    }
    std::vector<const char*> ptrs;
    for (const auto& s : items)
    {
        ptrs.push_back(s.c_str());
    }
    // Ashita's current_item is 0-based like ImGui's.
    const bool r = ImGui::Combo(luaL_checkstring(L, 1), &cur, ptrs.data(), static_cast<int>(ptrs.size()), optInt(L, 5, -1));
    refSetNum(L, 2, cur);
    lua_pushboolean(L, r);
    return 1;
}
int SliderFloat(lua_State* L)
{
    float      v = refGet<float>(L, 2, 0);
    const bool r = ImGui::SliderFloat(luaL_checkstring(L, 1), &v, optFloat(L, 3), optFloat(L, 4, 1), luaL_optstring(L, 5, "%.3f"), optInt(L, 6));
    refSetNum(L, 2, v);
    lua_pushboolean(L, r);
    return 1;
}
int SliderInt(lua_State* L)
{
    int        v = refGet<int>(L, 2, 0);
    const bool r = ImGui::SliderInt(luaL_checkstring(L, 1), &v, optInt(L, 3), optInt(L, 4, 100), luaL_optstring(L, 5, "%d"), optInt(L, 6));
    refSetNum(L, 2, v);
    lua_pushboolean(L, r);
    return 1;
}
int DragFloat(lua_State* L)
{
    float      v = refGet<float>(L, 2, 0);
    const bool r = ImGui::DragFloat(luaL_checkstring(L, 1), &v, optFloat(L, 3, 1), optFloat(L, 4), optFloat(L, 5), luaL_optstring(L, 6, "%.3f"), optInt(L, 7));
    refSetNum(L, 2, v);
    lua_pushboolean(L, r);
    return 1;
}
int DragInt(lua_State* L)
{
    int        v = refGet<int>(L, 2, 0);
    const bool r = ImGui::DragInt(luaL_checkstring(L, 1), &v, optFloat(L, 3, 1), optInt(L, 4), optInt(L, 5), luaL_optstring(L, 6, "%d"), optInt(L, 7));
    refSetNum(L, 2, v);
    lua_pushboolean(L, r);
    return 1;
}
// InputText: buffer { 'text' } and a size; the edited text goes back into [1].
int inputText(lua_State* L, int bufIdx, int sizeIdx, int flagsIdx, const char* label, const char* hint, ImVec2 multiline, bool isMulti)
{
    std::string text;
    if (lua_istable(L, bufIdx))
    {
        lua_rawgeti(L, bufIdx, 1);
        if (lua_isstring(L, -1))
        {
            text = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }
    const auto        size = static_cast<std::size_t>(std::max(optInt(L, sizeIdx, 256), 1));
    std::vector<char> buf(std::max(size, text.size() + 1), '\0');
    std::memcpy(buf.data(), text.data(), text.size());
    const int  flags = optInt(L, flagsIdx);
    const bool r     = isMulti ? ImGui::InputTextMultiline(label, buf.data(), buf.size(), multiline, flags)
                       : hint  ? ImGui::InputTextWithHint(label, hint, buf.data(), buf.size(), flags)
                               : ImGui::InputText(label, buf.data(), buf.size(), flags);
    if (lua_istable(L, bufIdx))
    {
        lua_pushstring(L, buf.data());
        lua_rawseti(L, bufIdx, 1);
    }
    lua_pushboolean(L, r);
    return 1;
}
int InputText(lua_State* L)
{
    return inputText(L, 2, 3, 4, luaL_checkstring(L, 1), nullptr, {}, false);
}
int InputTextMultiline(lua_State* L)
{
    return inputText(L, 2, 3, 5, luaL_checkstring(L, 1), nullptr, vec2(L, 4), true);
}
int InputTextWithHint(lua_State* L)
{
    return inputText(L, 3, 4, 5, luaL_checkstring(L, 1), luaL_optstring(L, 2, ""), {}, false);
}
int InputFloat(lua_State* L)
{
    float      v = refGet<float>(L, 2, 0);
    const bool r = ImGui::InputFloat(luaL_checkstring(L, 1), &v, optFloat(L, 3), optFloat(L, 4), luaL_optstring(L, 5, "%.3f"), optInt(L, 6));
    refSetNum(L, 2, v);
    lua_pushboolean(L, r);
    return 1;
}
int InputInt(lua_State* L)
{
    int        v = refGet<int>(L, 2, 0);
    const bool r = ImGui::InputInt(luaL_checkstring(L, 1), &v, optInt(L, 3, 1), optInt(L, 4, 100), optInt(L, 5));
    refSetNum(L, 2, v);
    lua_pushboolean(L, r);
    return 1;
}
// ColorEdit3/4, ColorPicker: the colour table's [1..4] are edited in place.
int colorEdit(lua_State* L, int n, bool picker)
{
    float c[4] = { 1, 1, 1, 1 };
    if (lua_istable(L, 2))
    {
        for (int k = 0; k < n; ++k)
        {
            lua_rawgeti(L, 2, k + 1);
            c[k] = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : c[k];
            lua_pop(L, 1);
        }
    }
    const char* label = luaL_checkstring(L, 1);
    const int   flags = optInt(L, 3);
    const bool  r     = picker ? (n == 4 ? ImGui::ColorPicker4(label, c, flags) : ImGui::ColorPicker3(label, c, flags))
                                 : (n == 4 ? ImGui::ColorEdit4(label, c, flags) : ImGui::ColorEdit3(label, c, flags));
    if (lua_istable(L, 2))
    {
        for (int k = 0; k < n; ++k)
        {
            lua_pushnumber(L, c[k]);
            lua_rawseti(L, 2, k + 1);
        }
    }
    lua_pushboolean(L, r);
    return 1;
}
int ColorEdit3(lua_State* L)
{
    return colorEdit(L, 3, false);
}
int ColorEdit4(lua_State* L)
{
    return colorEdit(L, 4, false);
}
int ColorPicker3(lua_State* L)
{
    return colorEdit(L, 3, true);
}
int ColorPicker4(lua_State* L)
{
    return colorEdit(L, 4, true);
}
int ColorButton(lua_State* L)
{
    lua_pushboolean(L, ImGui::ColorButton(luaL_checkstring(L, 1), vec4(L, 2), optInt(L, 3), vec2(L, 4)));
    return 1;
}
int TreeNode(lua_State* L)
{
    lua_pushboolean(L, lua_gettop(L) >= 2 ? ImGui::TreeNode(luaL_checkstring(L, 1), "%s", luaL_optstring(L, 2, ""))
                                          : ImGui::TreeNode(luaL_checkstring(L, 1)));
    return 1;
}
int TreeNodeEx(lua_State* L)
{
    lua_pushboolean(L, lua_gettop(L) >= 3 ? ImGui::TreeNodeEx(luaL_checkstring(L, 1), optInt(L, 2), "%s", luaL_optstring(L, 3, ""))
                                          : ImGui::TreeNodeEx(luaL_checkstring(L, 1), optInt(L, 2)));
    return 1;
}
int TreePush(lua_State* L)
{
    ImGui::TreePush(luaL_optstring(L, 1, "##tree"));
    return 0;
}
int CollapsingHeader(lua_State* L)
{
    if (lua_istable(L, 2))
    {
        bool       vis = refGet<bool>(L, 2, true);
        const bool r   = ImGui::CollapsingHeader(luaL_checkstring(L, 1), &vis, optInt(L, 3));
        refSetBool(L, 2, vis);
        lua_pushboolean(L, r);
    }
    else
    {
        lua_pushboolean(L, ImGui::CollapsingHeader(luaL_checkstring(L, 1), optInt(L, 2)));
    }
    return 1;
}
int SetNextItemOpen(lua_State* L)
{
    ImGui::SetNextItemOpen(lua_toboolean(L, 1) != 0, optInt(L, 2));
    return 0;
}
int Selectable(lua_State* L)
{
    if (lua_istable(L, 2))
    {
        bool       sel = refGet<bool>(L, 2, false);
        const bool r   = ImGui::Selectable(luaL_checkstring(L, 1), &sel, optInt(L, 3), vec2(L, 4));
        refSetBool(L, 2, sel);
        lua_pushboolean(L, r);
    }
    else
    {
        lua_pushboolean(L, ImGui::Selectable(luaL_checkstring(L, 1), lua_toboolean(L, 2) != 0, optInt(L, 3), vec2(L, 4)));
    }
    return 1;
}
int BeginListBox(lua_State* L)
{
    lua_pushboolean(L, ImGui::BeginListBox(luaL_checkstring(L, 1), vec2(L, 2)));
    return 1;
}
int PlotLines(lua_State* L)
{
    std::vector<float> v;
    if (lua_istable(L, 2))
    {
        const int n = optInt(L, 3, static_cast<int>(lua_objlen(L, 2)));
        for (int k = 1; k <= n; ++k)
        {
            lua_rawgeti(L, 2, k);
            v.push_back(static_cast<float>(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
    }
    ImGui::PlotLines(luaL_checkstring(L, 1), v.data(), static_cast<int>(v.size()), optInt(L, 4), optStr(L, 5), optFloat(L, 6, FLT_MAX), optFloat(L, 7, FLT_MAX), vec2(L, 8));
    return 0;
}
int PlotHistogram(lua_State* L)
{
    std::vector<float> v;
    if (lua_istable(L, 2))
    {
        const int n = optInt(L, 3, static_cast<int>(lua_objlen(L, 2)));
        for (int k = 1; k <= n; ++k)
        {
            lua_rawgeti(L, 2, k);
            v.push_back(static_cast<float>(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
    }
    ImGui::PlotHistogram(luaL_checkstring(L, 1), v.data(), static_cast<int>(v.size()), optInt(L, 4), optStr(L, 5), optFloat(L, 6, FLT_MAX), optFloat(L, 7, FLT_MAX), vec2(L, 8));
    return 0;
}
int BeginMenu(lua_State* L)
{
    lua_pushboolean(L, ImGui::BeginMenu(luaL_checkstring(L, 1), lua_isnoneornil(L, 2) || lua_toboolean(L, 2)));
    return 1;
}
int MenuItem(lua_State* L)
{
    const bool enabled = lua_isnoneornil(L, 4) || lua_toboolean(L, 4);
    if (lua_istable(L, 3))
    {
        bool       sel = refGet<bool>(L, 3, false);
        const bool r   = ImGui::MenuItem(luaL_checkstring(L, 1), optStr(L, 2), &sel, enabled);
        refSetBool(L, 3, sel);
        lua_pushboolean(L, r);
    }
    else
    {
        lua_pushboolean(L, ImGui::MenuItem(luaL_checkstring(L, 1), optStr(L, 2), lua_toboolean(L, 3) != 0, enabled));
    }
    return 1;
}
int SetTooltip(lua_State* L)
{
    ImGui::SetTooltip("%s", luaL_optstring(L, 1, ""));
    return 0;
}
int SetItemTooltip(lua_State* L)
{
    ImGui::SetItemTooltip("%s", luaL_optstring(L, 1, ""));
    return 0;
}
int BeginPopup(lua_State* L)
{
    lua_pushboolean(L, ImGui::BeginPopup(luaL_checkstring(L, 1), optInt(L, 2)));
    return 1;
}
int BeginPopupModal(lua_State* L)
{
    bool       open = refGet<bool>(L, 2, true);
    const bool r    = ImGui::BeginPopupModal(luaL_checkstring(L, 1), lua_istable(L, 2) ? &open : nullptr, optInt(L, 3));
    refSetBool(L, 2, open);
    lua_pushboolean(L, r);
    return 1;
}
int OpenPopup(lua_State* L)
{
    ImGui::OpenPopup(luaL_checkstring(L, 1), optInt(L, 2));
    return 0;
}
int BeginPopupContextItem(lua_State* L)
{
    lua_pushboolean(L, ImGui::BeginPopupContextItem(optStr(L, 1), optInt(L, 2, 1)));
    return 1;
}
int BeginPopupContextWindow(lua_State* L)
{
    lua_pushboolean(L, ImGui::BeginPopupContextWindow(optStr(L, 1), optInt(L, 2, 1)));
    return 1;
}
int IsPopupOpen(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsPopupOpen(luaL_checkstring(L, 1), optInt(L, 2)));
    return 1;
}
int BeginTable(lua_State* L)
{
    lua_pushboolean(L, ImGui::BeginTable(luaL_checkstring(L, 1), std::max(optInt(L, 2, 1), 1), optInt(L, 3), vec2(L, 4), optFloat(L, 5)));
    return 1;
}
int TableNextRow(lua_State* L)
{
    ImGui::TableNextRow(optInt(L, 1), optFloat(L, 2));
    return 0;
}
int TableSetColumnIndex(lua_State* L)
{
    lua_pushboolean(L, ImGui::TableSetColumnIndex(optInt(L, 1)));
    return 1;
}
int TableSetupColumn(lua_State* L)
{
    ImGui::TableSetupColumn(optStr(L, 1), optInt(L, 2), optFloat(L, 3), static_cast<ImGuiID>(optInt(L, 4)));
    return 0;
}
int TableSetupScrollFreeze(lua_State* L)
{
    ImGui::TableSetupScrollFreeze(optInt(L, 1), optInt(L, 2));
    return 0;
}
int TableHeader(lua_State* L)
{
    ImGui::TableHeader(luaL_optstring(L, 1, ""));
    return 0;
}
int TableSetBgColor(lua_State* L)
{
    ImGui::TableSetBgColor(optInt(L, 1), ImGui::ColorConvertFloat4ToU32(vec4(L, 2)), optInt(L, 3, -1));
    return 0;
}
int Columns(lua_State* L)
{
    ImGui::Columns(optInt(L, 1, 1), optStr(L, 2), lua_isnoneornil(L, 3) || lua_toboolean(L, 3));
    return 0;
}
int GetColumnWidth(lua_State* L)
{
    lua_pushnumber(L, ImGui::GetColumnWidth(optInt(L, 1, -1)));
    return 1;
}
int SetColumnWidth(lua_State* L)
{
    ImGui::SetColumnWidth(optInt(L, 1), optFloat(L, 2));
    return 0;
}
int BeginTabBar(lua_State* L)
{
    lua_pushboolean(L, ImGui::BeginTabBar(luaL_checkstring(L, 1), optInt(L, 2)));
    return 1;
}
int BeginTabItem(lua_State* L)
{
    bool       open = refGet<bool>(L, 2, true);
    const bool r    = ImGui::BeginTabItem(luaL_checkstring(L, 1), lua_istable(L, 2) ? &open : nullptr, optInt(L, 3));
    refSetBool(L, 2, open);
    lua_pushboolean(L, r);
    return 1;
}
int BeginDisabled(lua_State* L)
{
    ImGui::BeginDisabled(lua_isnoneornil(L, 1) || lua_toboolean(L, 1));
    return 0;
}
int PushClipRect(lua_State* L)
{
    ImGui::PushClipRect(vec2(L, 1), vec2(L, 2), lua_toboolean(L, 3) != 0);
    return 0;
}
int SetKeyboardFocusHere(lua_State* L)
{
    ImGui::SetKeyboardFocusHere(optInt(L, 1));
    return 0;
}
int IsItemHovered(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsItemHovered(optInt(L, 1)));
    return 1;
}
int IsItemClicked(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsItemClicked(optInt(L, 1)));
    return 1;
}
int IsMouseDown(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsMouseDown(optInt(L, 1)));
    return 1;
}
int IsMouseClicked(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsMouseClicked(optInt(L, 1), lua_toboolean(L, 2) != 0));
    return 1;
}
int IsMouseReleased(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsMouseReleased(optInt(L, 1)));
    return 1;
}
int IsMouseDoubleClicked(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsMouseDoubleClicked(optInt(L, 1)));
    return 1;
}
int IsMouseHoveringRect(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsMouseHoveringRect(vec2(L, 1), vec2(L, 2), lua_isnoneornil(L, 3) || lua_toboolean(L, 3)));
    return 1;
}
int IsMouseDragging(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsMouseDragging(optInt(L, 1), optFloat(L, 2, -1)));
    return 1;
}
int GetMouseDragDelta(lua_State* L)
{
    pushVec2(L, ImGui::GetMouseDragDelta(optInt(L, 1), optFloat(L, 2, -1)));
    return 2;
}
int ResetMouseDragDelta(lua_State* L)
{
    ImGui::ResetMouseDragDelta(optInt(L, 1));
    return 0;
}
int IsKeyDown(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsKeyDown(static_cast<ImGuiKey>(optInt(L, 1))));
    return 1;
}
int IsKeyPressed(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsKeyPressed(static_cast<ImGuiKey>(optInt(L, 1)), lua_isnoneornil(L, 2) || lua_toboolean(L, 2)));
    return 1;
}
int IsKeyReleased(lua_State* L)
{
    lua_pushboolean(L, ImGui::IsKeyReleased(static_cast<ImGuiKey>(optInt(L, 1))));
    return 1;
}
int CalcTextSize(lua_State* L)
{
    pushVec2(L, ImGui::CalcTextSize(luaL_optstring(L, 1, ""), nullptr, lua_toboolean(L, 2) != 0, optFloat(L, 3, -1)));
    return 2;
}
int ColorConvertU32ToFloat4(lua_State* L)
{
    const ImVec4 c = ImGui::ColorConvertU32ToFloat4(static_cast<ImU32>(lua_tonumber(L, 1)));
    lua_createtable(L, 4, 0);
    const float v[4] = { c.x, c.y, c.z, c.w };
    for (int k = 0; k < 4; ++k)
    {
        lua_pushnumber(L, v[k]);
        lua_rawseti(L, -2, k + 1);
    }
    return 1;
}
int ColorConvertFloat4ToU32(lua_State* L)
{
    lua_pushnumber(L, ImGui::ColorConvertFloat4ToU32(vec4(L, 1)));
    return 1;
}
int GetColorU32(lua_State* L)
{
    if (lua_istable(L, 1))
    {
        lua_pushnumber(L, ImGui::GetColorU32(vec4(L, 1)));
    }
    else
    {
        lua_pushnumber(L, ImGui::GetColorU32(optInt(L, 1), optFloat(L, 2, 1)));
    }
    return 1;
}
int GetClipboardText(lua_State* L)
{
    lua_pushstring(L, ImGui::GetClipboardText());
    return 1;
}
int SetClipboardText(lua_State* L)
{
    ImGui::SetClipboardText(luaL_optstring(L, 1, ""));
    return 0;
}
int SetNextFrameWantCaptureMouse(lua_State* L)
{
    ImGui::SetNextFrameWantCaptureMouse(lua_toboolean(L, 1) != 0);
    return 0;
}
int SetNextFrameWantCaptureKeyboard(lua_State* L)
{
    ImGui::SetNextFrameWantCaptureKeyboard(lua_toboolean(L, 1) != 0);
    return 0;
}
int Image(lua_State* L)
{
    // Textures are D3D8 objects in Ashita; the space is kept.
    ImGui::Dummy(vec2(L, 2));
    return 0;
}
int StyleColorsDark(lua_State*)
{
    ImGui::StyleColorsDark();
    return 0;
}
int StyleColorsLight(lua_State*)
{
    ImGui::StyleColorsLight();
    return 0;
}
int StyleColorsClassic(lua_State*)
{
    ImGui::StyleColorsClassic();
    return 0;
}
int GetVersion(lua_State* L)
{
    lua_pushstring(L, ImGui::GetVersion());
    return 1;
}

// GetStyle(): a table whose fields read and write ImGuiStyle. ImVec2 fields come as { x, y, [1], [2] }; Colors is a
// table of colours by ImGuiCol_ (0-based) whose entries write through too.
enum class FieldType
{
    Float,
    Vec2,
    Bool,
    Int,
};
struct StyleField
{
    const char* name;
    std::size_t offset;
    FieldType   type;
};
#define XI_F(N) { #N, offsetof(ImGuiStyle, N), FieldType::Float }
#define XI_V(N) { #N, offsetof(ImGuiStyle, N), FieldType::Vec2 }
#define XI_B(N) { #N, offsetof(ImGuiStyle, N), FieldType::Bool }
#define XI_I(N) { #N, offsetof(ImGuiStyle, N), FieldType::Int }
// Every public ImGuiStyle member (generated from imgui.h's list).
const StyleField kStyleFields[] = {
    XI_F(FontSizeBase), XI_F(FontScaleMain), XI_F(FontScaleDpi), XI_F(Alpha), XI_F(DisabledAlpha),
    XI_V(WindowPadding), XI_F(WindowRounding), XI_F(WindowBorderSize), XI_F(WindowBorderHoverPadding),
    XI_V(WindowMinSize), XI_V(WindowTitleAlign), XI_I(WindowMenuButtonPosition), XI_F(ChildRounding),
    XI_F(ChildBorderSize), XI_F(PopupRounding), XI_F(PopupBorderSize), XI_V(FramePadding), XI_F(FrameRounding),
    XI_F(FrameBorderSize), XI_V(ItemSpacing), XI_V(ItemInnerSpacing), XI_V(CellPadding), XI_V(TouchExtraPadding),
    XI_F(IndentSpacing), XI_F(ColumnsMinSpacing), XI_F(ScrollbarSize), XI_F(ScrollbarRounding),
    XI_F(ScrollbarPadding), XI_F(GrabMinSize), XI_F(GrabRounding), XI_F(LogSliderDeadzone), XI_F(ImageRounding),
    XI_F(ImageBorderSize), XI_F(TabRounding), XI_F(TabBorderSize), XI_F(TabMinWidthBase), XI_F(TabMinWidthShrink),
    XI_F(TabCloseButtonMinWidthSelected), XI_F(TabCloseButtonMinWidthUnselected), XI_F(TabBarBorderSize),
    XI_F(TabBarOverlineSize), XI_F(TableAngledHeadersAngle), XI_V(TableAngledHeadersTextAlign), XI_I(TreeLinesFlags),
    XI_F(TreeLinesSize), XI_F(TreeLinesRounding), XI_F(MenuItemRounding), XI_F(SelectableRounding),
    XI_F(DragDropTargetRounding), XI_F(DragDropTargetBorderSize), XI_F(DragDropTargetPadding), XI_F(ColorMarkerSize),
    XI_I(ColorButtonPosition), XI_V(ButtonTextAlign), XI_V(SelectableTextAlign), XI_F(InputTextCursorSize),
    XI_F(SeparatorSize), XI_F(SeparatorTextBorderSize), XI_V(SeparatorTextAlign), XI_V(SeparatorTextPadding),
    XI_V(DisplayWindowPadding), XI_V(DisplaySafeAreaPadding), XI_F(MouseCursorScale), XI_B(AntiAliasedLines),
    XI_B(AntiAliasedLinesUseTex), XI_B(AntiAliasedFill), XI_F(CurveTessellationTol),
    XI_F(CircleTessellationMaxError), XI_F(HoverStationaryDelay), XI_F(HoverDelayShort), XI_F(HoverDelayNormal),
    XI_I(HoverFlagsForTooltipMouse), XI_I(HoverFlagsForTooltipNav),
};
#undef XI_F
#undef XI_V
#undef XI_B
#undef XI_I

void pushVec2Table(lua_State* L, ImVec2 v)
{
    lua_createtable(L, 2, 2);
    lua_pushnumber(L, v.x);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, 1);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y);
    lua_pushvalue(L, -1);
    lua_rawseti(L, -3, 2);
    lua_setfield(L, -2, "y");
}
void pushVec4Table(lua_State* L, const ImVec4& v)
{
    lua_createtable(L, 4, 4);
    const float       f[4] = { v.x, v.y, v.z, v.w };
    const char* const n[4] = { "x", "y", "z", "w" };
    for (int k = 0; k < 4; ++k)
    {
        lua_pushnumber(L, f[k]);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, k + 1);
        lua_setfield(L, -2, n[k]);
    }
}
ImVec4 readVec4(lua_State* L, int i)
{
    if (lua_istable(L, i))
    {
        lua_getfield(L, i, "x");
        const bool named = lua_isnumber(L, -1);
        lua_pop(L, 1);
        if (named)
        {
            float       f[4] = { 0, 0, 0, 1 };
            const char* n[4] = { "x", "y", "z", "w" };
            for (int k = 0; k < 4; ++k)
            {
                lua_getfield(L, i, n[k]);
                f[k] = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : f[k];
                lua_pop(L, 1);
            }
            return { f[0], f[1], f[2], f[3] };
        }
    }
    return vec4(L, i);
}
int colorsIndex(lua_State* L)
{
    const int c = static_cast<int>(luaL_checkinteger(L, 2));
    if (c < 0 || c >= ImGuiCol_COUNT)
    {
        lua_pushnil(L);
        return 1;
    }
    pushVec4Table(L, ImGui::GetStyle().Colors[c]);
    return 1;
}
int colorsNewIndex(lua_State* L)
{
    const int c = static_cast<int>(luaL_checkinteger(L, 2));
    if (c >= 0 && c < ImGuiCol_COUNT)
    {
        ImGui::GetStyle().Colors[c] = readVec4(L, 3);
    }
    return 0;
}
int styleIndex(lua_State* L)
{
    auto*             s = reinterpret_cast<char*>(&ImGui::GetStyle());
    const std::string k = luaL_checkstring(L, 2);
    for (const auto& f : kStyleFields)
    {
        if (k == f.name)
        {
            switch (f.type)
            {
                case FieldType::Vec2: pushVec2Table(L, *reinterpret_cast<ImVec2*>(s + f.offset)); break;
                case FieldType::Bool: lua_pushboolean(L, *reinterpret_cast<bool*>(s + f.offset)); break;
                case FieldType::Int: lua_pushnumber(L, *reinterpret_cast<int*>(s + f.offset)); break;
                case FieldType::Float: lua_pushnumber(L, static_cast<double>(*reinterpret_cast<float*>(s + f.offset))); break;
            }
            return 1;
        }
    }
    if (k == "Colors")
    {
        lua_newtable(L);
        lua_createtable(L, 0, 2);
        lua_pushcfunction(L, colorsIndex);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, colorsNewIndex);
        lua_setfield(L, -2, "__newindex");
        lua_setmetatable(L, -2);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}
int styleNewIndex(lua_State* L)
{
    auto*             s = reinterpret_cast<char*>(&ImGui::GetStyle());
    const std::string k = luaL_checkstring(L, 2);
    for (const auto& f : kStyleFields)
    {
        if (k == f.name)
        {
            if (f.type == FieldType::Bool)
            {
                *reinterpret_cast<bool*>(s + f.offset) = lua_toboolean(L, 3) != 0;
            }
            else if (f.type == FieldType::Int)
            {
                *reinterpret_cast<int*>(s + f.offset) = optInt(L, 3);
            }
            else if (f.type == FieldType::Vec2)
            {
                ImVec2 v = vec2(L, 3);
                if (lua_istable(L, 3))
                {
                    lua_getfield(L, 3, "x");
                    lua_getfield(L, 3, "y");
                    v.x = lua_isnumber(L, -2) ? static_cast<float>(lua_tonumber(L, -2)) : v.x;
                    v.y = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : v.y;
                    lua_pop(L, 2);
                }
                *reinterpret_cast<ImVec2*>(s + f.offset) = v;
            }
            else
            {
                *reinterpret_cast<float*>(s + f.offset) = optFloat(L, 3);
            }
            return 0;
        }
    }
    return 0;
}
int GetStyle(lua_State* L)
{
    lua_newtable(L);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, styleIndex);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, styleNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    return 1;
}
int GetStyleColorVec4(lua_State* L)
{
    const int c = optInt(L, 1);
    if (c < 0 || c >= ImGuiCol_COUNT)
    {
        lua_pushnil(L);
        return 1;
    }
    pushVec4Table(L, ImGui::GetStyle().Colors[c]);
    return 1;
}
int ShowStyleEditor(lua_State*)
{
    ImGui::ShowStyleEditor();
    return 0;
}
int ShowDemoWindow(lua_State* L)
{
    bool open = refGet<bool>(L, 1, true);
    ImGui::ShowDemoWindow(lua_istable(L, 1) ? &open : nullptr);
    refSetBool(L, 1, open);
    return 0;
}

// Draw lists (GetWindowDrawList / GetForegroundDrawList / GetBackgroundDrawList): AddLine, AddRect, AddRectFilled,
// AddText, AddCircle, AddCircleFilled, AddTriangleFilled on the list kept in the table's upvalue.
ImDrawList* drawListOf(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "__list");
    auto* dl = static_cast<ImDrawList*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return dl;
}
ImU32 colArg(lua_State* L, int i)
{
    return lua_istable(L, i) ? ImGui::ColorConvertFloat4ToU32(vec4(L, i)) : static_cast<ImU32>(lua_tonumber(L, i));
}
int dlAddLine(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->AddLine(vec2(L, 2), vec2(L, 3), colArg(L, 4), optFloat(L, 5, 1));
    return 0;
}
int dlAddRect(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->AddRect(vec2(L, 2), vec2(L, 3), colArg(L, 4), optFloat(L, 5), optInt(L, 6), optFloat(L, 7, 1));
    return 0;
}
int dlAddRectFilled(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->AddRectFilled(vec2(L, 2), vec2(L, 3), colArg(L, 4), optFloat(L, 5), optInt(L, 6));
    return 0;
}
int dlAddText(lua_State* L)
{
    auto* dl = drawListOf(L);
    if (!dl)
    {
        return 0;
    }
    if (lua_istable(L, 2))
    {
        dl->AddText(vec2(L, 2), colArg(L, 3), luaL_optstring(L, 4, ""));
    }
    else
    {
        // AddText(font, size, pos, col, text [, wrap_width]): the font from AddFontFromFileTTF, or nil.
        auto* font = lua_islightuserdata(L, 2) ? static_cast<ImFont*>(lua_touserdata(L, 2)) : nullptr;
        dl->AddText(font, optFloat(L, 3, 0), vec2(L, 4), colArg(L, 5), luaL_optstring(L, 6, ""), nullptr, optFloat(L, 7, 0));
    }
    return 0;
}
int dlPushClipRect(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->PushClipRect(vec2(L, 2), vec2(L, 3), lua_toboolean(L, 4) != 0);
    return 0;
}
int dlPopClipRect(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->PopClipRect();
    return 0;
}
int dlPathClear(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->PathClear();
    return 0;
}
int dlPathLineTo(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->PathLineTo(vec2(L, 2));
    return 0;
}
int dlPathArcTo(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->PathArcTo(vec2(L, 2), optFloat(L, 3), optFloat(L, 4), optFloat(L, 5), optInt(L, 6));
    return 0;
}
int dlPathFillConvex(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->PathFillConvex(colArg(L, 2));
    return 0;
}
int dlPathStroke(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->PathStroke(colArg(L, 2), optInt(L, 3), optFloat(L, 4, 1));
    return 0;
}
int dlAddRectFilledMultiColor(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->AddRectFilledMultiColor(vec2(L, 2), vec2(L, 3), colArg(L, 4), colArg(L, 5), colArg(L, 6), colArg(L, 7));
    return 0;
}
int dlAddQuadFilled(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->AddQuadFilled(vec2(L, 2), vec2(L, 3), vec2(L, 4), vec2(L, 5), colArg(L, 6));
    return 0;
}
int dlAddTriangle(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->AddTriangle(vec2(L, 2), vec2(L, 3), vec2(L, 4), colArg(L, 5), optFloat(L, 6, 1));
    return 0;
}
int dlAddCircle(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->AddCircle(vec2(L, 2), optFloat(L, 3), colArg(L, 4), optInt(L, 5), optFloat(L, 6, 1));
    return 0;
}
int dlAddCircleFilled(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->AddCircleFilled(vec2(L, 2), optFloat(L, 3), colArg(L, 4), optInt(L, 5));
    return 0;
}
int dlAddTriangleFilled(lua_State* L)
{
    if (auto* dl = drawListOf(L))
        dl->AddTriangleFilled(vec2(L, 2), vec2(L, 3), vec2(L, 4), colArg(L, 5));
    return 0;
}
int pushDrawList(lua_State* L, ImDrawList* dl)
{
    lua_createtable(L, 0, 8);
    lua_pushlightuserdata(L, dl);
    lua_setfield(L, -2, "__list");
    const luaL_Reg fns[] = { { "AddLine", dlAddLine },
                             { "AddRect", dlAddRect },
                             { "AddRectFilled", dlAddRectFilled },
                             { "AddText", dlAddText },
                             { "AddCircle", dlAddCircle },
                             { "AddCircleFilled", dlAddCircleFilled },
                             { "AddTriangleFilled", dlAddTriangleFilled },
                             { "AddTriangle", dlAddTriangle },
                             { "AddQuadFilled", dlAddQuadFilled },
                             { "AddRectFilledMultiColor", dlAddRectFilledMultiColor },
                             { "PushClipRect", dlPushClipRect },
                             { "PopClipRect", dlPopClipRect },
                             { "PathClear", dlPathClear },
                             { "PathLineTo", dlPathLineTo },
                             { "PathArcTo", dlPathArcTo },
                             { "PathFillConvex", dlPathFillConvex },
                             { "PathStroke", dlPathStroke },
                             { nullptr, nullptr } };
    for (const auto* f = fns; f->name; ++f)
    {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
    return 1;
}
int GetWindowDrawList(lua_State* L)
{
    return pushDrawList(L, ImGui::GetWindowDrawList());
}
int GetForegroundDrawList(lua_State* L)
{
    return pushDrawList(L, ImGui::GetForegroundDrawList());
}
int GetBackgroundDrawList(lua_State* L)
{
    return pushDrawList(L, ImGui::GetBackgroundDrawList());
}
// io.KeysDown / io.KeysData as Ashita's Win32 backend indexes them: by Windows virtual-key code (imgui_impl_win32's
// legacy key index). The keys ImGui tracks are answered from its own state; others read as up.
ImGuiKey imguiKeyOfVk(int vk)
{
    if (vk >= 'A' && vk <= 'Z')
    {
        return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    }
    if (vk >= '0' && vk <= '9')
    {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    }
    if (vk >= 0x70 && vk <= 0x7B)
    {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - 0x70));
    }
    if (vk >= 0x60 && vk <= 0x69)
    {
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - 0x60));
    }
    switch (vk)
    {
        case 0x08: return ImGuiKey_Backspace;
        case 0x09: return ImGuiKey_Tab;
        case 0x0D: return ImGuiKey_Enter;
        case 0x10: return ImGuiMod_Shift;
        case 0x11: return ImGuiMod_Ctrl;
        case 0x12: return ImGuiMod_Alt;
        case 0x1B: return ImGuiKey_Escape;
        case 0x20: return ImGuiKey_Space;
        case 0x21: return ImGuiKey_PageUp;
        case 0x22: return ImGuiKey_PageDown;
        case 0x23: return ImGuiKey_End;
        case 0x24: return ImGuiKey_Home;
        case 0x25: return ImGuiKey_LeftArrow;
        case 0x26: return ImGuiKey_UpArrow;
        case 0x27: return ImGuiKey_RightArrow;
        case 0x28: return ImGuiKey_DownArrow;
        case 0x2D: return ImGuiKey_Insert;
        case 0x2E: return ImGuiKey_Delete;
        default: return ImGuiKey_None;
    }
}
bool vkDown(lua_State* L)
{
    const auto key = imguiKeyOfVk(static_cast<int>(luaL_optinteger(L, 2, -1)));
    return key != ImGuiKey_None && ImGui::IsKeyDown(key);
}
int keysDownIndex(lua_State* L)
{
    lua_pushboolean(L, vkDown(L));
    return 1;
}
int keysDataIndex(lua_State* L)
{
    lua_createtable(L, 0, 1);
    lua_pushboolean(L, vkDown(L));
    lua_setfield(L, -2, "Down");
    return 1;
}
void pushKeyTable(lua_State* L, lua_CFunction index, const char* name)
{
    lua_newtable(L);
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, name);
}
int GetIO(lua_State* L)
{
    const ImGuiIO& io = ImGui::GetIO();
    lua_createtable(L, 0, 8);
    // An ImVec2 as Ashita's bindings give it (.x / .y), and as a list (x, y) for the addons that index it.
    lua_createtable(L, 2, 2);
    lua_pushnumber(L, io.DisplaySize.x);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, io.DisplaySize.y);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, io.DisplaySize.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, io.DisplaySize.y);
    lua_setfield(L, -2, "y");
    lua_setfield(L, -2, "DisplaySize");
    lua_pushnumber(L, io.Framerate);
    lua_setfield(L, -2, "Framerate");
    lua_pushnumber(L, io.DeltaTime);
    lua_setfield(L, -2, "DeltaTime");
    lua_pushboolean(L, io.WantCaptureMouse);
    lua_setfield(L, -2, "WantCaptureMouse");
    lua_pushboolean(L, io.WantCaptureKeyboard);
    lua_setfield(L, -2, "WantCaptureKeyboard");
    lua_pushboolean(L, io.KeyCtrl);
    lua_setfield(L, -2, "KeyCtrl");
    lua_pushboolean(L, io.KeyShift);
    lua_setfield(L, -2, "KeyShift");
    lua_pushboolean(L, io.KeyAlt);
    lua_setfield(L, -2, "KeyAlt");
    // An ImVec2 as Ashita's bindings give it (.x / .y), and as a list (x, y) for the addons that index it.
    lua_createtable(L, 2, 2);
    lua_pushnumber(L, io.MousePos.x);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, io.MousePos.y);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, io.MousePos.x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, io.MousePos.y);
    lua_setfield(L, -2, "y");
    lua_setfield(L, -2, "MousePos");
    pushKeyTable(L, keysDownIndex, "KeysDown");
    pushKeyTable(L, keysDataIndex, "KeysData");
    return 1;
}

#define XI_FN(NAME) { #NAME, NAME }
const luaL_Reg kFunctions[] = {
    XI_FN(Begin), XI_FN(End), XI_FN(BeginChild), XI_FN(EndChild), XI_FN(IsWindowAppearing), XI_FN(IsWindowCollapsed),
    XI_FN(IsWindowFocused), XI_FN(IsWindowHovered), XI_FN(GetWindowPos), XI_FN(GetWindowSize), XI_FN(GetWindowWidth),
    XI_FN(GetWindowHeight), XI_FN(SetNextWindowPos), XI_FN(SetNextWindowSize), XI_FN(SetNextWindowSizeConstraints),
    XI_FN(SetNextWindowContentSize), XI_FN(SetNextWindowCollapsed), XI_FN(SetNextWindowFocus), XI_FN(SetNextWindowBgAlpha),
    XI_FN(SetWindowPos), XI_FN(SetWindowSize), XI_FN(SetWindowFocus), XI_FN(SetWindowFontScale), XI_FN(GetScrollX), XI_FN(GetScrollY),
    XI_FN(SetScrollX), XI_FN(SetScrollY), XI_FN(GetScrollMaxX), XI_FN(GetScrollMaxY), XI_FN(SetScrollHereX), XI_FN(SetScrollHereY),
    XI_FN(PushFont), XI_FN(GetFont), XI_FN(AddFontFromFileTTF), XI_FN(PopFont), XI_FN(GetFontSize), XI_FN(PushStyleColor), XI_FN(PopStyleColor), XI_FN(PushStyleVar),
    XI_FN(PushStyleVarX), XI_FN(PushStyleVarY), XI_FN(PopStyleVar), XI_FN(PushItemFlag), XI_FN(PopItemFlag), XI_FN(PushItemWidth),
    XI_FN(PopItemWidth), XI_FN(SetNextItemWidth), XI_FN(CalcItemWidth), XI_FN(PushTextWrapPos), XI_FN(PopTextWrapPos),
    XI_FN(GetCursorScreenPos), XI_FN(SetCursorScreenPos), XI_FN(GetContentRegionAvail), XI_FN(GetCursorPos), XI_FN(GetCursorPosX),
    XI_FN(GetCursorPosY), XI_FN(SetCursorPos), XI_FN(SetCursorPosX), XI_FN(SetCursorPosY), XI_FN(GetCursorStartPos), XI_FN(Separator),
    XI_FN(SameLine), XI_FN(NewLine), XI_FN(Spacing), XI_FN(Dummy), XI_FN(Indent), XI_FN(Unindent), XI_FN(BeginGroup), XI_FN(EndGroup),
    XI_FN(AlignTextToFramePadding), XI_FN(GetTextLineHeight), XI_FN(GetTextLineHeightWithSpacing), XI_FN(GetFrameHeight),
    XI_FN(GetFrameHeightWithSpacing), XI_FN(PushID), XI_FN(PopID), XI_FN(GetID), { "TextUnformatted", Text }, XI_FN(Text),
    XI_FN(TextColored), XI_FN(TextDisabled), XI_FN(TextWrapped), XI_FN(LabelText), XI_FN(BulletText), XI_FN(SeparatorText),
    XI_FN(Button), XI_FN(SmallButton), XI_FN(InvisibleButton), XI_FN(ArrowButton), XI_FN(Checkbox), XI_FN(RadioButton),
    XI_FN(ProgressBar), XI_FN(Bullet), XI_FN(Image), XI_FN(BeginCombo), XI_FN(EndCombo), XI_FN(Combo), XI_FN(DragFloat), XI_FN(DragInt),
    XI_FN(SliderFloat), XI_FN(SliderInt), XI_FN(InputText), XI_FN(InputTextMultiline), XI_FN(InputTextWithHint), XI_FN(InputFloat),
    XI_FN(InputInt), XI_FN(ColorEdit3), XI_FN(ColorEdit4), XI_FN(ColorPicker3), XI_FN(ColorPicker4), XI_FN(ColorButton), XI_FN(TreeNode),
    XI_FN(TreeNodeEx), XI_FN(TreePush), XI_FN(TreePop), XI_FN(GetTreeNodeToLabelSpacing), XI_FN(CollapsingHeader), XI_FN(SetNextItemOpen),
    XI_FN(Selectable), XI_FN(BeginListBox), XI_FN(EndListBox), XI_FN(PlotLines), XI_FN(PlotHistogram), XI_FN(BeginMenuBar),
    XI_FN(EndMenuBar), XI_FN(BeginMainMenuBar), XI_FN(EndMainMenuBar), XI_FN(BeginMenu), XI_FN(EndMenu), XI_FN(MenuItem),
    XI_FN(BeginTooltip), XI_FN(EndTooltip), XI_FN(BeginItemTooltip), XI_FN(SetTooltip), XI_FN(SetItemTooltip), XI_FN(BeginPopup),
    XI_FN(BeginPopupModal), XI_FN(EndPopup), XI_FN(OpenPopup), XI_FN(CloseCurrentPopup), XI_FN(BeginPopupContextItem),
    XI_FN(BeginPopupContextWindow), XI_FN(IsPopupOpen), XI_FN(BeginTable), XI_FN(EndTable), XI_FN(TableNextRow), XI_FN(TableNextColumn),
    XI_FN(TableSetColumnIndex), XI_FN(TableSetupColumn), XI_FN(TableSetupScrollFreeze), XI_FN(TableHeader), XI_FN(TableHeadersRow),
    XI_FN(TableGetColumnCount), XI_FN(TableGetColumnIndex), XI_FN(TableGetRowIndex), XI_FN(TableSetBgColor), XI_FN(Columns),
    XI_FN(NextColumn), XI_FN(GetColumnIndex), XI_FN(GetColumnWidth), XI_FN(SetColumnWidth), XI_FN(GetColumnsCount), XI_FN(BeginTabBar),
    XI_FN(EndTabBar), XI_FN(BeginTabItem), XI_FN(EndTabItem), XI_FN(BeginDisabled), XI_FN(EndDisabled), XI_FN(PushClipRect),
    XI_FN(PopClipRect), XI_FN(SetItemDefaultFocus), XI_FN(SetKeyboardFocusHere), XI_FN(IsItemHovered), XI_FN(IsItemActive),
    XI_FN(IsItemFocused), XI_FN(IsItemClicked), XI_FN(IsItemVisible), XI_FN(IsItemEdited), XI_FN(IsItemActivated),
    XI_FN(IsItemDeactivated), XI_FN(IsItemDeactivatedAfterEdit), XI_FN(IsItemToggledOpen), XI_FN(IsAnyItemHovered),
    XI_FN(IsAnyItemActive), XI_FN(IsAnyItemFocused), XI_FN(GetItemRectMin), XI_FN(GetItemRectMax), XI_FN(GetItemRectSize),
    XI_FN(GetTime), XI_FN(GetFrameCount), XI_FN(IsKeyDown), XI_FN(IsKeyPressed), XI_FN(IsKeyReleased), XI_FN(IsMouseDown),
    XI_FN(IsMouseClicked), XI_FN(IsMouseReleased), XI_FN(IsMouseDoubleClicked), XI_FN(IsMouseHoveringRect), XI_FN(IsAnyMouseDown),
    XI_FN(GetMousePos), XI_FN(IsMouseDragging), XI_FN(GetMouseDragDelta), XI_FN(ResetMouseDragDelta), XI_FN(CalcTextSize),
    XI_FN(ColorConvertU32ToFloat4), XI_FN(ColorConvertFloat4ToU32), XI_FN(GetColorU32), XI_FN(GetClipboardText), XI_FN(SetClipboardText),
    XI_FN(SetNextFrameWantCaptureMouse), XI_FN(SetNextFrameWantCaptureKeyboard), XI_FN(GetStyle), XI_FN(GetStyleColorVec4), XI_FN(ShowStyleEditor), XI_FN(ShowDemoWindow), XI_FN(GetIO), XI_FN(StyleColorsDark),
    XI_FN(StyleColorsLight), XI_FN(StyleColorsClassic), XI_FN(GetVersion), XI_FN(GetWindowDrawList), XI_FN(GetForegroundDrawList),
    XI_FN(GetBackgroundDrawList), XI_FN(BeginDragDropTarget), XI_FN(EndDragDropTarget), XI_FN(EndDragDropSource),
    { nullptr, nullptr },
};
#undef XI_FN

} // namespace

void imguiFrameStart(lua_State* L)
{
    depth()[L] = { 0, 0 };
}

void imguiClose(lua_State* L)
{
    depth().erase(L);
}

void openImgui(lua_State* L)
{
    lua_newtable(L);
    for (const auto* f = kFunctions; f->name; ++f)
    {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
    lua_setglobal(L, "__xi_imgui");
}

} // namespace xi::ashita

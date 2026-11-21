// SPDX-License-Identifier: GPL-3.0-or-later
// Game data the Ashita resource manager reads beyond ashita::Resources (lua_state.cpp). Process-wide: the tables are the
// game's own and the same for every addon state.
#pragma once

namespace xi::dat
{
class DatIndex;
} // namespace xi::dat

namespace xi::ashita
{

// The install the resource manager reads on demand (must outlive the addon states; nullptr clears it):
//  - monster TP move names, IResourceManager's 'monsters.abilities': file 7035 (ROM/27/80), a dialog table indexed by
//    the move's id less 256 (index 1 "Foot Kick" = LSB / retail mob skill 257);
//  - status icons for IStatusIcon.Bitmap: file 87.
// Unset, those answer nil.
void setGameData(const dat::DatIndex* index);

} // namespace xi::ashita

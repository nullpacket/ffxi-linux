// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace xi::client
{

// The Config font colour (cfg 35..49) of a battle message's log line, by message id (MsgBasic, < 1024) and whether the
// target or the actor is us; 0 = one of the fixed colours (not in Config). The game picks a log mode from seven
// per-message tables by who acted on whom (CODE 0x1009e630) and the mode's colour code (table 0x1036e898); the
// colour only differs between "target is you", "actor is you" and the rest (docs/ui-format-notes.md §8).
uint8_t battleColour(uint16_t message, bool targetIsUs, bool actorIsUs);

// Config Misc. "damage display" (cfg 19): a battle message's pop-up kind, 1 damage (`damv`), 2 recovery (`curv`),
// 3 miss / evade / no effect (`misv`), 0 none (CODE byte table 0x1035caf0, filter 0x100ad820).
uint8_t damageCategory(uint16_t message);

} // namespace xi::client

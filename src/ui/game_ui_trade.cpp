// SPDX-License-Identifier: GPL-3.0-or-later
// Player trade: "menu trade" (ours), "menu gift" (the partner's), "menu itemctrl" (how many) and "menu moneyctr"
// (gil), as the game's trade manager runs them (ui-format-notes §7.10; CODE 0x10151510 and its windows).
#include "ui/game_ui.h"

#include "ui/layout_view.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>

namespace xi::ui
{

namespace
{

constexpr int kOk = 9, kCancel = 10, kGil = 11; // "menu trade" elements; 1..8 are the boxes

std::vector<std::byte> tradeRes(uint32_t kind, uint16_t counter = 0)
{
    // C2S 0x033 TRADE_RES {Kind u32, TradeCounter u16}: 0 start, 1 cancel, 2 confirm, 3 confirm withdrawn.
    std::vector<std::byte> pkt(8);
    std::memcpy(pkt.data(), &kind, 4);
    std::memcpy(pkt.data() + 4, &counter, 2);
    return pkt;
}

} // namespace

void GameUi::tradeRequest(const client::Entity* t, const GameUiData& d, const GameUiActions& act)
{
    // CODE 0x10151a00: no target logs standard message 29 "Select a target to trade with.", a target that is not a
    // player 28 "You cannot trade with that target."; a player who asked us is answered (0x033 Start), anyone else is
    // asked (0x032 TRADE_REQ {UniqueNo, ActIndex}) and string 20 of table 10 "You offer to trade with %s." is logged.
    stack_.clear();
    if (!act.send)
    {
        return;
    }
    if (!t)
    {
        if (act.stdNote) act.stdNote(29, {});
        return;
    }
    if (!t->isPc)
    {
        if (act.stdNote) act.stdNote(28, {});
        return;
    }
    const auto& w = *d.world;
    if (w.trade.requestFrom == t->id)
    {
        act.send(0x033, tradeRes(0));
        return;
    }
    std::vector<std::byte> pkt(8);
    std::memcpy(pkt.data(), &t->id, 4);
    std::memcpy(pkt.data() + 4, &t->index, 2);
    act.send(0x032, std::move(pkt));
    if (d.formats && act.note)
    {
        std::string text(d.formats->get(20));
        if (const auto at = text.find("%s"); at != std::string::npos)
        {
            text.replace(at, 2, t->name);
        }
        act.note(text);
    }
}

void GameUi::tradeSetBox(int box, uint8_t slot, uint16_t itemId, uint32_t count, const GameUiActions& act)
{
    // CODE 0x101510d0: when locked, withdraw the confirm first (0x033 Kind 3), then 0x034 for the box (count 0
    // empties it); both sides go back to editing.
    if (tradeLocked_ && act.send)
    {
        act.send(0x033, tradeRes(3));
    }
    tradeLocked_                                = false;
    tradeBoxes_[static_cast<std::size_t>(box)] = { count ? slot : uint8_t{ 0 }, count };
    if (!act.send)
    {
        return;
    }
    std::vector<std::byte> pkt(8);
    std::memcpy(pkt.data(), &count, 4);
    std::memcpy(pkt.data() + 4, &itemId, 2);
    pkt[6] = static_cast<std::byte>(slot);
    pkt[7] = static_cast<std::byte>(box + 1); // TradeIndex 0 is the gil slot; items use 1..8
    act.send(0x034, std::move(pkt));
}

void GameUi::tradeSetGil(uint32_t gil, const GameUiActions& act)
{
    // The gil box: 0x034 {ItemNum = gil, ItemNo 0xFFFF, ItemIndex 0, TradeIndex 0} (callback 0x101514f0).
    if (tradeLocked_ && act.send)
    {
        act.send(0x033, tradeRes(3));
    }
    tradeLocked_ = false;
    tradeGil_    = gil;
    if (!act.send)
    {
        return;
    }
    std::vector<std::byte> pkt(8);
    std::memcpy(pkt.data(), &gil, 4);
    const uint16_t gilId = 0xFFFF;
    std::memcpy(pkt.data() + 4, &gilId, 2);
    act.send(0x034, std::move(pkt));
}

void GameUi::tradePick(int box, uint8_t slot, uint16_t itemId, uint32_t have, const GameUiData& d, const GameUiActions& act)
{
    // CODE 0x10150f20: an item already offered goes back into its own box; a stack asks how many ("menu itemctrl",
    // 1 .. the stack, starting at 1), a single item goes in at once.
    (void)d;
    for (std::size_t b = 0; b < tradeBoxes_.size(); ++b)
    {
        if (tradeBoxes_[b].first == slot && tradeBoxes_[b].second > 0)
        {
            box = static_cast<int>(b);
        }
    }
    if (have <= 1)
    {
        tradeSetBox(box, slot, itemId, 1, act);
        return;
    }
    tradeCtrl_ = { 1, have, 1, 0, slot, itemId };
    open(Kind::TradeQty, box);
}

void GameUi::tradeCancel(const GameUiData& d, const GameUiActions& act)
{
    // Cancel (and the range check): 0x033 Kind 1, then the windows close at once; the server tells only the partner.
    if (act.send)
    {
        act.send(0x033, tradeRes(1));
    }
    tradeClosedSerial_ = d.world->trade.serial;
    tradeLocked_       = false;
    stack_.clear();
}

void GameUi::tradeConfirm(Menu& m, const GameUiData& d, const GameUiActions& act)
{
    const auto& w = *d.world;
    if (m.kind == Kind::TradeQty || m.kind == Kind::TradeGil)
    {
        const auto v    = tradeCtrl_.value;
        const auto kind = m.kind;
        const int  box  = m.param;
        stack_.pop_back();
        if (kind == Kind::TradeQty)
        {
            tradeSetBox(box, tradeCtrl_.slot, tradeCtrl_.item, v, act);
        }
        else
        {
            tradeSetGil(v, act);
        }
        return;
    }
    // "menu trade": confirm on OK locks our side (0x033 Kind 2, state 4); while locked any other confirm moves the
    // cursor to OK. Cancel ends the trade. The gil box opens the gil window, an empty box the item list, a filled box
    // empties it.
    if (m.cursor == kCancel)
    {
        tradeCancel(d, act);
        return;
    }
    if (m.cursor == kOk)
    {
        if (act.send && !tradeLocked_)
        {
            act.send(0x033, tradeRes(2, w.trade.counter));
            tradeLocked_     = true;
            tradeLockMine_   = w.trade.mineUpdates;
            tradeLockTheirs_ = w.trade.theirsUpdates;
        }
        return;
    }
    if (tradeLocked_)
    {
        m.cursor = kOk;
        return;
    }
    if (m.cursor == kGil)
    {
        uint32_t have = 0;
        for (const auto& it : w.inventory)
        {
            if (it.container == 0 && it.slot == 0)
            {
                have = it.count;
            }
        }
        tradeCtrl_ = { tradeGil_, have, 1, 0, 0, 0 };
        open(Kind::TradeGil);
        return;
    }
    if (m.cursor >= 1 && m.cursor <= 8)
    {
        const auto b = static_cast<std::size_t>(m.cursor - 1);
        if (const auto [slot, count] = tradeBoxes_[b]; slot != 0 && count > 0)
        {
            uint16_t item = 0;
            for (const auto& it : w.inventory)
            {
                item = it.container == 0 && it.slot == slot ? it.itemId : item;
            }
            tradeSetBox(m.cursor - 1, slot, item, 0, act);
            return;
        }
        open(Kind::TradeItem, m.cursor - 1);
    }
}

bool GameUi::tradeApply(Menu& m, Cmd c, const GameUiData& d, const GameUiActions& act)
{
    if (m.kind == Kind::TradeQty || m.kind == Kind::TradeGil)
    {
        auto& t = tradeCtrl_;
        if (c == Cmd::Confirm)
        {
            tradeConfirm(m, d, act);
            return true;
        }
        if (c == Cmd::Back)
        {
            stack_.pop_back(); // callback(-1): back to the trade window, nothing changes
            return true;
        }
        if (m.kind == Kind::TradeQty)
        {
            // CODE 0x1013a5f0: Down -step (at least 1), Up +step (up to the stack), Left 1, Right the stack; holding a
            // key long enough raises the step from 1 to 10 (0x1013a540: 60 ticks; here 30 repeats, INFERRED).
            static Cmd last = Cmd::Confirm;
            t.held          = c == last ? t.held + 1 : 0;
            last            = c;
            t.step          = t.held >= 30 ? 10 : 1;
            if (c == Cmd::Down) t.value = t.value > t.step ? t.value - t.step : 1;
            else if (c == Cmd::Up) t.value = std::min(t.max, t.value + t.step);
            else if (c == Cmd::Left) t.value = 1;
            else if (c == Cmd::Right) t.value = t.max;
            return true;
        }
        // CODE 0x10140af0: a digit cursor (step 1, 10, 100...): Down -step when the value allows, Up +step capped at
        // what we hold; Left a digit lower (at the units: the value goes to 0), Right a digit higher while there are
        // digits left (else the value goes to the maximum).
        if (c == Cmd::Down && t.value >= t.step) t.value -= t.step;
        else if (c == Cmd::Up) t.value = std::min<uint64_t>(t.max, static_cast<uint64_t>(t.value) + t.step) & 0xFFFFFFFFu;
        else if (c == Cmd::Left)
        {
            if (t.step > 1) t.step /= 10;
            else t.value = 0;
        }
        else if (c == Cmd::Right)
        {
            if (t.step <= t.max / 10 && t.max / t.step >= 10) t.step *= 10;
            else t.value = t.max;
        }
        return true;
    }
    if (m.kind != Kind::PlayerTrade)
    {
        return false;
    }
    if (c == Cmd::Confirm)
    {
        tradeConfirm(m, d, act);
        return true;
    }
    if (c == Cmd::Back)
    {
        m.cursor = kCancel; // Back moves to Cancel; it does not end the trade
        return true;
    }
    if (tradeLocked_)
    {
        // Locked: the cursor stays on OK / Cancel, Up / Down step between them.
        if (c == Cmd::Up || c == Cmd::Down)
        {
            m.cursor = m.cursor == kOk ? kCancel : kOk;
        }
        return true;
    }
    return c == Cmd::PageUp || c == Cmd::PageDown; // no count keys here: the arrows follow the layout
}

void GameUi::tradeSync(const GameUiData& d, const GameUiActions& act)
{
    // The window follows the server: 0x022 Start opens it, cancel / done / errors close it. A trade this client
    // closed itself (Cancel, out of range) stays closed until the next Start.
    const auto& t     = d.world->trade;
    const bool  open_ = std::any_of(stack_.begin(), stack_.end(), [](const Menu& m) { return m.kind == Kind::PlayerTrade; });
    if (t.active && !open_ && t.serial != tradeClosedSerial_)
    {
        stack_.clear();
        tradeBoxes_   = {};
        tradeGil_     = 0;
        tradeLocked_  = false;
        tradeEchoSeen_ = t.mineUpdates;
        open(Kind::PlayerTrade);
    }
    else if (!t.active && open_)
    {
        stack_.clear();
        tradeLocked_ = false;
    }
    if (!t.active)
    {
        return;
    }
    // Our boxes follow the server's echo (0x025), so refused items (Ex, Rare the partner owns) drop back out.
    if (t.mineUpdates != tradeEchoSeen_)
    {
        tradeEchoSeen_ = t.mineUpdates;
        for (std::size_t b = 0; b < 8; ++b)
        {
            tradeBoxes_[b] = t.mine[b + 1];
        }
        tradeGil_ = t.mine[0].second;
    }
    // A change to either offer unlocks both sides (CODE 0x10152380; LSB resets both accepts).
    if (tradeLocked_ && (t.theirsUpdates != tradeLockTheirs_ || t.mineUpdates != tradeLockMine_))
    {
        tradeLocked_ = false;
    }
    // CODE 0x101517e0 / 0x101523c0: 6 yalms or more (squared 3D distance >= 36.0) cancels and closes.
    if (open_)
    {
        for (const auto& e : d.world->entities)
        {
            if (e.id == t.partner)
            {
                const float dx = e.pos.x - d.playerX, dy = e.pos.y - d.playerY, dz = e.pos.z - d.playerZ;
                if (dx * dx + dy * dy + dz * dz >= 36.0f)
                {
                    tradeCancel(d, act);
                }
                break;
            }
        }
    }
}

} // namespace xi::ui

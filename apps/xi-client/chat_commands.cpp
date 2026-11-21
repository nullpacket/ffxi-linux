// SPDX-License-Identifier: GPL-3.0-or-later
#include "chat_commands.h"

#include "client/target_lock.h"

namespace xi::app
{

const char* jobName(uint8_t j)
{
    static const char* names[] = { "---", "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
                                   "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN" };
    return j < std::size(names) ? names[j] : "?";
}

void runChatLine(ChatContext& c, const std::string& line, uint8_t kind)
{
    auto& [session, world, uiActions, searchJob, clientText, args, zoneMaps, usageText, sound, namesHidden, selfEntity, scene, quitting, outerTarget, lockOn, keyItemNames, hideTrusts, ignoreTrusts, ignorePet, followId, feet, configFlags, blacklistAsked, battleText, autorun, areaNames, bugReport] = c;

                            // A picked sub-target (<id:HEX>, GameUi's <st> cursor) stands in for the target here: commands
                            // that act on the target use it, and chat lines name it.
                            std::string           t      = line;
                            const client::Entity* target = outerTarget;
                            if (const auto at = t.find("<id:"); at != std::string::npos)
                            {
                                const auto end = t.find('>', at);
                                const auto id  = static_cast<uint32_t>(std::strtoul(t.c_str() + at + 4, nullptr, 16));
                                target         = id == world.charId ? &selfEntity : nullptr;
                                for (const auto& e : world.entities)
                                {
                                    if (e.id == id)
                                    {
                                        target = &e;
                                    }
                                }
                                t.replace(at, end == std::string::npos ? std::string::npos : end - at + 1, target ? "<t>" : "");
                                while (!t.empty() && t.back() == ' ')
                                {
                                    t.pop_back();
                                }
                            }
                            // Client messages (d_msg 55648): 101 "Spelling of name is incorrect.", 102 "Failed to execute
                            // command.", 120 "...A command error occurred.".
                            auto clientMsg = [&](std::size_t id, const char* fallback) {
                                std::string m = clientText && !clientText->get(id).empty() ? std::string(clientText->get(id)) : std::string(fallback);
                                while (!m.empty() && static_cast<unsigned char>(m.back()) < 0x20)
                                {
                                    m.pop_back(); // some entries end in a newline
                                }
                                session->note(std::move(m));
                            };
                            const auto        sp   = t.find(' ');
                            const std::string cmd  = t.substr(0, sp);
                            const std::string arg  = sp == std::string::npos ? std::string() : t.substr(sp + 1);
                            const auto        me   = gfx::ZoneScene::renderToGame(feet);
                            auto byName = [&](const std::string& n) -> const client::Entity* {
                                for (const auto& e : world.entities)
                                {
                                    if (e.isPc && strcasecmp(e.name.c_str(), n.c_str()) == 0)
                                    {
                                        return &e;
                                    }
                                }
                                return nullptr;
                            };
                            // The nearest entity of a kind (within 30 yalms, alive, named): the /target* commands.
                            auto nearest = [&](auto pred) -> const client::Entity* {
                                const client::Entity* best = nullptr;
                                float                 bd   = 30.0f;
                                for (const auto& e : world.entities)
                                {
                                    const float dd = glm::distance(e.pos, me);
                                    if (!e.name.empty() && e.hpp > 0 && !e.targetOff && pred(e) && dd < bd)
                                    {
                                        best = &e;
                                        bd   = dd;
                                    }
                                }
                                return best;
                            };
                            // Chat tokens, as the game expands them: <t> target, <me>, <hp> <hpp> <mp> <mpp> <tp>, <pos>
                            // (the map grid square, 32-pixel cells inside the map's 32-pixel border: "(I-9)").
                            auto expandTokens = [&](std::string text) {
                                auto        sub  = [&](const std::string& token, const std::string& value) {
                                    for (std::size_t p = text.find(token); p != std::string::npos; p = text.find(token, p + value.size()))
                                    {
                                        text.replace(p, token.size(), value);
                                    }
                                };
                                const auto pct = [](uint32_t v, int32_t max) { return std::to_string(max > 0 ? v * 100 / static_cast<uint32_t>(max) : 0); };
                                sub("<me>", world.charName);
                                sub("<t>", target ? target->name : world.charName);
                                sub("<hpp>", pct(world.hp, world.stats.hpMax));
                                sub("<mpp>", pct(world.mp, world.stats.mpMax));
                                sub("<hp>", std::to_string(world.hp));
                                sub("<mp>", std::to_string(world.mp));
                                sub("<tp>", std::to_string(world.tp));
                                if (text.find("<pos>") != std::string::npos)
                                {
                                    std::string pos = "(?)";
                                    const auto  mid = scene ? static_cast<int8_t>(scene->mapIdAt(feet.x, feet.z, feet.y + 0.5f)) : int8_t{ 0 };
                                    if (const auto* rec = zoneMaps.find(world.zone, mid))
                                    {
                                        const auto [u, v] = rec->project(me.x, me.z);
                                        const int col = static_cast<int>((u - 32) / 32), row = static_cast<int>((v - 32) / 32);
                                        if (col >= 0 && col < 15 && row >= 0 && row < 15)
                                        {
                                            pos = fmt::format("({}-{})", static_cast<char>('A' + col), row + 1);
                                        }
                                    }
                                    sub("<pos>", pos);
                                }
                                return text;
                            };
                            if (cmd == "/bugreport" || cmd == "/bug")
                            {
                                bugReport = arg; // ClientApp::stepBugReport gathers and files it
                                return;
                            }
                            if (cmd == "/echo")
                            {
                                session->note(arg);
                                return;
                            }
                            if ((cmd == "/emote" || cmd == "/em") && !arg.empty())
                            {
                                session->say(arg, 8); // MESSAGE_EMOTION
                                return;
                            }
                            if ((cmd == "/unity" || cmd == "/u") && !arg.empty())
                            {
                                session->say(arg, 33); // MESSAGE_UNITY
                                return;
                            }
                            if ((cmd == "/assistj" || cmd == "/aj" || cmd == "/assiste" || cmd == "/ae") && !arg.empty())
                            {
                                session->say(arg, cmd[cmd.size() - 1] == 'j' ? 0x22 : 0x23); // assist channel (Japanese / English)
                                return;
                            }
                            if (cmd == "/random")
                            {
                                // C2S 0x0A2 DICE {u32 the number given to /random}; the server rolls and announces it.
                                std::vector<std::byte> pkt(4);
                                const uint32_t         n = arg.empty() ? 0u : static_cast<uint32_t>(std::strtoul(arg.c_str(), nullptr, 10));
                                std::memcpy(pkt.data(), &n, 4);
                                session->send(0x0A2, std::move(pkt));
                                return;
                            }
                            if (cmd == "/targetnpc" || cmd == "/targetbnpc" || cmd == "/targetpc")
                            {
                                const client::Entity* e = cmd == "/targetpc"   ? nearest([&](const client::Entity& x) { return x.isPc && x.id != world.charId; })
                                                        : cmd == "/targetbnpc" ? nearest([](const client::Entity& x) { return x.isMob; })
                                                                               : nearest([](const client::Entity& x) { return !x.isPc && !x.isMob; });
                                if (e)
                                {
                                    uiActions.setTarget(e->id);
                                }
                                return;
                            }
                            if (cmd == "/attackoff")
                            {
                                if (world.engaged())
                                {
                                    session->action(target ? target->id : 0, target ? target->index : 0, 4); // disengage
                                }
                                return;
                            }
                            if (cmd == "/ra" || cmd == "/range" || cmd == "/shoot" || cmd == "/throw")
                            {
                                if (target && !target->isPc)
                                {
                                    session->action(target->id, target->index, 0x10); // ranged attack
                                }
                                return;
                            }
                            if (cmd == "/callforhelp")
                            {
                                if (target && !target->isPc)
                                {
                                    session->action(target->id, target->index, 0x05); // call for help (LSB ActionCategory Help)
                                }
                                return;
                            }
                            if (cmd == "/sit")
                            {
                                session->send(0x0EA, std::vector<std::byte>(4)); // SIT {Mode 0 = toggle}
                                return;
                            }
                            if (cmd == "/jump")
                            {
                                std::vector<std::byte> pkt(8); // JUMP {UniqueNo, ActIndex}
                                std::memcpy(pkt.data(), &world.charId, 4);
                                std::memcpy(pkt.data() + 4, &world.actIndex, 2);
                                session->send(0x11D, std::move(pkt));
                                return;
                            }
                            if (cmd == "/checkname" || cmd == "/cn" || cmd == "/checkparam")
                            {
                                // EQUIP_INSPECT {UniqueNo, ActIndex u32, Kind (1 checkname, 2 checkparam)}; /checkparam on
                                // nothing checks yourself.
                                const bool     self = !target || (cmd == "/checkparam" && arg == "<me>");
                                const uint32_t id   = self ? world.charId : target->id;
                                const uint32_t idx  = self ? world.actIndex : target->index;
                                std::vector<std::byte> pkt(12);
                                std::memcpy(pkt.data(), &id, 4);
                                std::memcpy(pkt.data() + 4, &idx, 4);
                                pkt[8] = static_cast<std::byte>(cmd == "/checkparam" ? 2 : 1);
                                session->send(0x0DD, std::move(pkt));
                                return;
                            }
                            // /pcmd kick|leader <name>, /pcmd breakup; /invite (/inv) <name> = /pcmd add.
                            if (cmd == "/pcmd" && (arg.rfind("kick ", 0) == 0 || arg.rfind("leader ", 0) == 0))
                            {
                                const auto who = arg.substr(arg.find(' ') + 1);
                                bool       found = false;
                                for (const auto& m : world.party)
                                {
                                    if (strcasecmp(m.name.c_str(), who.c_str()) == 0)
                                    {
                                        found = true;
                                        std::vector<std::byte> pkt(arg[0] == 'k' ? 0x18 : 0x14);
                                        if (arg[0] == 'k') // GROUP_STRIKE {UniqueNo, ActIndex, Kind 0, pad, sName[15]}
                                        {
                                            std::memcpy(pkt.data(), &m.id, 4);
                                            std::memcpy(pkt.data() + 8, m.name.data(), std::min<std::size_t>(m.name.size(), 15));
                                            session->send(0x071, std::move(pkt));
                                        }
                                        else // GROUP_CHANGE2 {sName[16], Kind 0, ChangeKind 0 = leader}
                                        {
                                            std::memcpy(pkt.data(), m.name.data(), std::min<std::size_t>(m.name.size(), 15));
                                            session->send(0x077, std::move(pkt));
                                        }
                                    }
                                }
                                if (!found)
                                {
                                    clientMsg(101, "Spelling of name is incorrect.");
                                }
                                return;
                            }
                            // /acmd (/alliancecmd): the party commands with Kind 5 (alliance). "add" invites another
                            // party's leader; "kick" names the leader of the party to remove; "leader" passes the lead.
                            if (cmd == "/acmd" || cmd == "/alliancecmd")
                            {
                                const auto        sp2         = arg.find(' ');
                                const std::string sub         = arg.substr(0, sp2);
                                const std::string who         = sp2 == std::string::npos ? std::string() : arg.substr(sp2 + 1);
                                auto              groupPacket = [](std::size_t n) {
                                    std::vector<std::byte> pkt(n);
                                    pkt[0] = std::byte{ 5 };
                                    return pkt;
                                };
                                if (sub == "leave" || sub == "breakup")
                                {
                                    session->send(sub == "leave" ? 0x06F : 0x070, groupPacket(4)); // GROUP_LEAVE / GROUP_BREAKUP
                                    return;
                                }
                                if (sub == "add")
                                {
                                    if (const auto* e = byName(who))
                                    {
                                        std::vector<std::byte> pkt(8); // GROUP_SOLICIT_REQ {UniqueNo, ActIndex, Kind 5}
                                        std::memcpy(pkt.data(), &e->id, 4);
                                        std::memcpy(pkt.data() + 4, &e->index, 2);
                                        pkt[6] = std::byte{ 5 };
                                        session->send(0x06E, std::move(pkt));
                                    }
                                    else
                                    {
                                        clientMsg(101, "Spelling of name is incorrect.");
                                    }
                                    return;
                                }
                                if ((sub == "kick" || sub == "leader") && !who.empty())
                                {
                                    const client::PartyMember* m = nullptr;
                                    for (const auto& pm : world.party)
                                    {
                                        if (strcasecmp(pm.name.c_str(), who.c_str()) == 0)
                                        {
                                            m = &pm;
                                        }
                                    }
                                    if (!m)
                                    {
                                        clientMsg(101, "Spelling of name is incorrect.");
                                        return;
                                    }
                                    if (sub == "kick") // GROUP_STRIKE {UniqueNo, ActIndex, Kind 5, pad, sName[15]}
                                    {
                                        std::vector<std::byte> pkt(0x18);
                                        std::memcpy(pkt.data(), &m->id, 4);
                                        std::memcpy(pkt.data() + 4, &m->index, 2);
                                        pkt[6] = std::byte{ 5 };
                                        std::memcpy(pkt.data() + 8, m->name.data(), std::min<std::size_t>(m->name.size(), 15));
                                        session->send(0x071, std::move(pkt));
                                    }
                                    else // GROUP_CHANGE2 {sName[16], Kind 5, ChangeKind 1 = alliance leader}
                                    {
                                        std::vector<std::byte> pkt(0x14);
                                        std::memcpy(pkt.data(), m->name.data(), std::min<std::size_t>(m->name.size(), 15));
                                        pkt[0x10] = std::byte{ 5 };
                                        pkt[0x11] = std::byte{ 1 };
                                        session->send(0x077, std::move(pkt));
                                    }
                                    return;
                                }
                                clientMsg(120, "...A command error occurred.");
                                return;
                            }
                            if (cmd == "/sitchair")
                            {
                                // C2S 0x113 SITCHAIR {u32 Mode (0 toggle, 1 on, 2 off), u32 ChairId}: chair n (0 = the plain
                                // one; others need their key item, else the server uses chair 0); "off" stands up.
                                std::vector<std::byte> pkt(8);
                                const uint32_t mode  = arg == "off" ? 2u : 0u;
                                const uint32_t chair = arg.empty() || arg == "off" ? 0u : static_cast<uint32_t>(std::clamp(std::atoi(arg.c_str()), 0, 20));
                                std::memcpy(pkt.data(), &mode, 4);
                                std::memcpy(pkt.data() + 4, &chair, 4);
                                session->send(0x113, std::move(pkt));
                                return;
                            }
                            if (cmd == "/pcmd" && arg == "breakup")
                            {
                                session->send(0x070, std::vector<std::byte>(4)); // GROUP_BREAKUP, Kind 0 = party
                                return;
                            }
                            if ((cmd == "/invite" || cmd == "/inv") && !arg.empty())
                            {
                                if (const auto* e = byName(arg))
                                {
                                    session->inviteToParty(e->id, e->index);
                                }
                                else
                                {
                                    clientMsg(101, "Spelling of name is incorrect.");
                                }
                                return;
                            }
                            // Config flags (C2S 0x0DC CONFIG: a flag bit, SetFlg 1 on / 2 off): /anon, /away, /autotarget,
                            // /displayhead, /invite (seeking a party). "on" / "off", else a toggle of what was last asked.
                            static const std::pair<const char*, std::pair<int, int>> kFlags[] = {
                                { "/anon", { 0, 2 } },       { "/anonymous", { 0, 2 } },  { "/away", { 0, 1 } }, { "/invite", { 0, 0 } },
                                { "/autotarget", { 1, 6 } }, { "/displayhead", { 3, 3 } }, { "/recruit", { 3, 5 } }, { "/rec", { 3, 5 } }
                            };
                            for (const auto& [name, at] : kFlags)
                            {
                                if (cmd == name)
                                {
                                    const std::string key = cmd == "/anonymous" ? "/anon" : cmd == "/rec" ? "/recruit" : cmd;
                                    bool&             cur = configFlags[key];
                                    // /autotarget and /displayhead name the off-flags: "on" means the flag off.
                                    const bool inverted = key == "/autotarget" || key == "/displayhead";
                                    const bool want     = arg == "on" ? !inverted : arg == "off" ? inverted : !cur;
                                    cur                 = want;
                                    std::vector<std::byte> pkt(16);
                                    pkt[static_cast<std::size_t>(at.first)] = static_cast<std::byte>(1 << at.second);
                                    pkt[12]                                  = static_cast<std::byte>(want ? 1 : 2);
                                    session->send(0x0DC, std::move(pkt));
                                    if (key == "/recruit")
                                    {
                                        session->note(want ? "Request to join is enabled." : "Request to join is disabled."); // no game text for it
                                    }
                                    return;
                                }
                            }
                            // /assist (/as) [name]: target what that player (default: the targeted player) targets
                            // (their facetarget index from 0x00D).
                            if (cmd == "/assist" || cmd == "/as")
                            {
                                const client::Entity* who = arg.empty() ? (target && target->isPc ? target : nullptr) : byName(arg);
                                const client::Entity* to  = nullptr;
                                for (const auto& e : world.entities)
                                {
                                    if (who && who->targetIndex != 0 && e.index == who->targetIndex)
                                    {
                                        to = &e;
                                    }
                                }
                                if (to)
                                {
                                    uiActions.setTarget(to->id);
                                }
                                else if (!who && !arg.empty())
                                {
                                    clientMsg(101, "Spelling of name is incorrect.");
                                }
                                return;
                            }
                            if (cmd == "/blockaid" || cmd == "/blockhelp")
                            {
                                // ACTION 0x18 BlockAid {StatusId: 0 off, 1 on, 2 toggle}.
                                session->action(world.charId, world.actIndex, 0x18, arg == "off" ? 0u : arg == "on" ? 1u : 2u);
                                return;
                            }
                            if (cmd == "/mount" || cmd == "/mo")
                            {
                                // ACTION 0x1A Mount {MountId}: the mount's key item is 3072 ("Chocobo companion") + MountId.
                                // No name: the first mount held.
                                std::optional<uint32_t> pick;
                                for (uint32_t id = 3072; id <= 3108 && !pick; ++id)
                                {
                                    const auto* ki = keyItemNames ? keyItemNames->find(id) : nullptr;
                                    if (!ki)
                                    {
                                        continue;
                                    }
                                    // Names are "<note glyph>Chocobo companion".
                                    std::string n  = ki->name;
                                    const auto  at = n.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
                                    n              = at == std::string::npos ? n : n.substr(at);
                                    if (const auto cut = n.rfind(" companion"); cut != std::string::npos)
                                    {
                                        n.resize(cut);
                                    }
                                    if (arg.empty() ? std::ranges::binary_search(world.keyItems, static_cast<uint16_t>(id))
                                                    : strncasecmp(n.c_str(), arg.c_str(), arg.size()) == 0)
                                    {
                                        pick = id - 3072;
                                    }
                                }
                                if (!pick)
                                {
                                    clientMsg(101, "Spelling of name is incorrect.");
                                    return;
                                }
                                session->action(world.charId, world.actIndex, 0x1A, *pick);
                                return;
                            }
                            if (cmd == "/lockstyle")
                            {
                                // C2S 0x053 LOCKSTYLE {Count, Mode, Flags, pad, Items[16] {ItemIndex, EquipKind, Category, pad,
                                // u16 ItemNo, pad}}. "on" locks today's gear (Mode 3 Set), "off" = Mode 0, none = Mode 2 (query).
                                std::vector<std::byte> pkt(4 + 16 * 8);
                                const uint8_t          mode = arg == "on" ? 3 : arg == "off" ? 0 : 2;
                                uint8_t                n    = 0;
                                if (mode == 3)
                                {
                                    for (uint8_t slot = 0; slot < 16; ++slot)
                                    {
                                        const auto [bag, at] = world.equipment[slot];
                                        if (at == 0)
                                        {
                                            continue;
                                        }
                                        for (const auto& it : world.inventory)
                                        {
                                            if (it.container == bag && it.slot == at)
                                            {
                                                auto* e = pkt.data() + 4 + 8 * n++;
                                                e[0]    = static_cast<std::byte>(at);
                                                e[1]    = static_cast<std::byte>(slot);
                                                e[2]    = static_cast<std::byte>(bag);
                                                std::memcpy(e + 4, &it.itemId, 2);
                                            }
                                        }
                                    }
                                }
                                pkt[0] = static_cast<std::byte>(n);
                                pkt[1] = static_cast<std::byte>(mode);
                                session->send(0x053, std::move(pkt));
                                if (mode != 2)
                                {
                                    // Config Misc. 3 "own equipment look" (cfg 191 = 1 fixed): re-sent at each zone-in.
                                    args.display_.gameConfig[191] = mode == 3 ? 1 : 0;
                                    if (const auto file = DisplaySettings::userFile())
                                    {
                                        DisplaySettings::saveOwn(*file, "cfg_191", mode == 3 ? 1 : 0);
                                    }
                                }
                                return;
                            }
                            // /search (/sea) [all] [job] [level-level] [name]: players in this area (or everywhere), from
                            // the search server (src/client/search.h). Asked off the frame loop; lines land in the log.
                            if (cmd == "/search" || cmd == "/sea")
                            {
                                if (searchJob.valid())
                                {
                                    return; // one search at a time
                                }
                                client::SearchQuery q;
                                q.zone = world.zone;
                                std::istringstream words(arg);
                                for (std::string w; words >> w;)
                                {
                                    const auto lw = [&] {
                                        std::string l = w;
                                        std::ranges::transform(l, l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                                        return l;
                                    }();
                                    int a = 0, b = 0;
                                    if (lw == "all")
                                    {
                                        q.zone.reset();
                                    }
                                    else if (std::sscanf(lw.c_str(), "%d-%d", &a, &b) == 2)
                                    {
                                        q.minLvl = static_cast<uint8_t>(std::clamp(a, 1, 99));
                                        q.maxLvl = static_cast<uint8_t>(std::clamp(b, 1, 99));
                                    }
                                    else if (const auto j = [&] {
                                                 for (uint8_t k = 1; k < 23; ++k)
                                                 {
                                                     if (strcasecmp(jobName(k), w.c_str()) == 0)
                                                     {
                                                         return k;
                                                     }
                                                 }
                                                 return uint8_t{ 0 };
                                             }();
                                             j != 0)
                                    {
                                        q.job = j;
                                    }
                                    else
                                    {
                                        q.name = w;
                                    }
                                }
                                const auto host  = args.host;
                                const auto port  = args.searchPort;
                                const auto found = clientText ? std::string(clientText->get(203)) : std::string("%d matches found (%d results displayed).");
                                auto       zoneName = [names = areaNames](uint16_t z) {
                                    const auto n = names ? names->get(z) : std::string_view();
                                    return n.empty() ? fmt::format("zone {}", z) : std::string(n);
                                };
                                searchJob = std::async(std::launch::async, [q, host, port, found, zoneName] {
                                    std::vector<std::string> lines;
                                    try
                                    {
                                        const auto r = client::searchPlayers(net::Endpoint::resolve(host, port), q);
                                        // "%d matches found (%d results displayed)." (55648 #203)
                                        std::string head = found;
                                        for (const auto v : { r.total, static_cast<uint32_t>(r.hits.size()) })
                                        {
                                            if (const auto at = head.find("%d"); at != std::string::npos)
                                            {
                                                head.replace(at, 2, std::to_string(v));
                                            }
                                        }
                                        lines.push_back(std::move(head));
                                        for (const auto& h : r.hits)
                                        {
                                            static const char* kComment[] = { "", "  [party]", "  [battle content]", "  [missions/quests]",
                                                                              "  [items]", "  [linkshell]", "  [friends]", "  [comment]" };
                                            const char* note = kComment[std::min<uint32_t>(h.commentType >> 4, 7)];
                                            lines.push_back((h.anonymous() ? fmt::format("  {}  {}  (anonymous)", h.name, zoneName(h.zone))
                                                             : h.subJob    ? fmt::format("  {}  {}  {}{}/{}{}", h.name, zoneName(h.zone), jobName(h.mainJob),
                                                                                         h.mainLevel, jobName(h.subJob), h.subLevel)
                                                                           : fmt::format("  {}  {}  {}{}", h.name, zoneName(h.zone), jobName(h.mainJob), h.mainLevel)) +
                                                            note);
                                        }
                                    }
                                    catch (const std::exception& e)
                                    {
                                        lines.push_back(fmt::format("Search failed: {}", e.what()));
                                    }
                                    return lines;
                                });
                                return;
                            }
                            // /blacklist (/blist) [add|delete <name>]: C2S 0x03D BLACK_EDIT {u32 ID, Name[16], i8 Mode 0 add /
                            // 1 remove}; no arguments lists it. The server answers with 0x042.
                            if (cmd == "/blacklist" || cmd == "/blist")
                            {
                                const auto        sp2 = arg.find(' ');
                                const std::string sub = arg.substr(0, sp2);
                                std::string       who = sp2 == std::string::npos ? std::string() : arg.substr(sp2 + 1);
                                std::erase(who, '"');
                                if ((sub == "add" || sub == "delete") && !who.empty())
                                {
                                    std::vector<std::byte> pkt(0x18);
                                    std::memcpy(pkt.data() + 4, who.data(), std::min<std::size_t>(who.size(), 15));
                                    blacklistAsked = sub == "add" ? 0 : 1;
                                    pkt[0x14]      = static_cast<std::byte>(blacklistAsked);
                                    session->send(0x03D, std::move(pkt));
                                    return;
                                }
                                if (!sub.empty())
                                {
                                    clientMsg(120, "...A command error occurred.");
                                    return;
                                }
                                session->note(fmt::format("Blacklist ({}):", world.blacklist.size()));
                                for (const auto& [id, n] : world.blacklist)
                                {
                                    session->note("  " + n);
                                }
                                return;
                            }
                            // /seacom (/sc) [category] <text>: the search comment (C2S 0x0E0 SET_USERMSG {sMessage[128], u32
                            // installTime, platform[4], u32 srvExCode, u32 cliExCode, u32 msgType}); no text clears it. The game
                            // picks the category from a menu; here a leading word does: seekparty, findmember, sell, buy, ls,
                            // recruit, friends (others otherwise).
                            if (cmd == "/seacom" || cmd == "/sc")
                            {
                                static const std::pair<const char*, uint32_t> kKinds[] = {
                                    { "seekparty", 0x11 }, { "findmember", 0x12 }, { "sell", 0x41 },    { "buy", 0x42 },
                                    { "ls", 0x51 },        { "recruit", 0x52 },    { "friends", 0x61 }, { "others", 0x73 },
                                };
                                std::string text = arg;
                                uint32_t    type = text.empty() ? 0u : 0x73u;
                                for (const auto& [k, v] : kKinds)
                                {
                                    const std::size_t n = std::strlen(k);
                                    if (strncasecmp(text.c_str(), k, n) == 0 && (text.size() == n || text[n] == ' '))
                                    {
                                        type = v;
                                        text = text.size() > n ? text.substr(n + 1) : std::string();
                                    }
                                }
                                std::vector<std::byte> pkt(0x94);
                                std::memcpy(pkt.data(), text.data(), std::min<std::size_t>(text.size(), 127));
                                std::memcpy(pkt.data() + 0x90, &type, 4);
                                session->send(0x0E0, std::move(pkt));
                                return;
                            }
                            // /nominate (/propose) <say|shout|party|linkshell|linkshell2> "question" "option" ...: C2S 0x0A0
                            // SWITCH_PROPOSAL {Kind, Str[128]} (the server splits the quoted words). Again: the vote ends.
                            if (cmd == "/nominate" || cmd == "/propose")
                            {
                                static const std::pair<const char*, uint8_t> kModes[] = {
                                    { "party", 1 }, { "p", 1 }, { "linkshell", 2 }, { "l", 2 }, { "linkshell2", 3 }, { "l2", 3 },
                                    { "say", 5 },   { "s", 5 }, { "shout", 6 },     { "sh", 6 },
                                };
                                const auto        sp2     = arg.find(' ');
                                const std::string mode    = arg.substr(0, sp2);
                                uint8_t           channel = 0;
                                for (const auto& [n, k] : kModes)
                                {
                                    if (strcasecmp(mode.c_str(), n) == 0)
                                    {
                                        channel = k;
                                    }
                                }
                                const std::string rest = sp2 == std::string::npos ? std::string() : arg.substr(sp2 + 1);
                                if (channel == 0 && !arg.empty())
                                {
                                    clientMsg(120, "...A command error occurred.");
                                    return;
                                }
                                std::vector<std::byte> pkt(0x84);
                                pkt[0] = static_cast<std::byte>(channel ? channel : 5);
                                std::memcpy(pkt.data() + 1, rest.data(), std::min<std::size_t>(rest.size(), 127));
                                session->send(0x0A0, std::move(pkt));
                                return;
                            }
                            // /vote [name] [number]: C2S 0x0A1 SWITCH_VOTE {Index, Name[15]}; no name = the latest proposal;
                            // no number shows the question again.
                            if (cmd == "/vote")
                            {
                                std::istringstream ws(arg);
                                std::string        a1, a2;
                                ws >> a1 >> a2;
                                std::string who = world.lastProposer;
                                int         n   = 0;
                                if (!a1.empty() && std::isdigit(static_cast<unsigned char>(a1[0])))
                                {
                                    n = std::atoi(a1.c_str());
                                }
                                else if (!a1.empty())
                                {
                                    who = a1;
                                    n   = a2.empty() ? 0 : std::atoi(a2.c_str());
                                }
                                if (who.empty())
                                {
                                    clientMsg(120, "...A command error occurred.");
                                    return;
                                }
                                if (n <= 0)
                                {
                                    session->note(fmt::format("{}'s proposal:", who));
                                    std::istringstream body(world.lastProposal);
                                    for (std::string l; std::getline(body, l);)
                                    {
                                        session->note("  " + l);
                                    }
                                    return;
                                }
                                std::vector<std::byte> pkt(0x10);
                                pkt[0] = static_cast<std::byte>(std::clamp(n, 1, 8));
                                std::memcpy(pkt.data() + 1, who.data(), std::min<std::size_t>(who.size(), 15));
                                session->send(0x0A1, std::move(pkt));
                                return;
                            }
                            if (cmd == "/servmes" || cmd == "/smes")
                            {
                                session->requestServerMessage();
                                return;
                            }
                            if (std::string reply; sound.command(cmd, arg, reply)) // /mutebgm /mutese /battlebgm /playse
                            {
                                session->note(reply);
                                return;
                            }
                            if (cmd == "/playtime" || cmd == "/playlog")
                            {
                                const auto secs = world.playTime + static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                                                    std::chrono::steady_clock::now() - world.playTimeAt).count());
                                session->note(fmt::format("Time Played: {}:{:02}:{:02}", secs / 3600, secs / 60 % 60, secs % 60)); // 55652 #110
                                return;
                            }
                            if (cmd == "/returnfaith" || cmd == "/refa" || cmd == "/returntrust" || cmd == "/retr")
                            {
                                // Releasing a trust is ACTION 0x00 (talk) on it. Trusts are the party members with entity ids
                                // (0x01xxxxxx), not character ids. "all" releases every one.
                                std::string n = arg;
                                std::erase(n, '"');
                                bool any = false;
                                for (const auto& m : world.party)
                                {
                                    if ((m.id >> 24) == 1 && m.partyNo == 0 &&
                                        (n == "all" || (!n.empty() && strncasecmp(m.name.c_str(), n.c_str(), n.size()) == 0)))
                                    {
                                        session->action(m.id, m.index, 0x00);
                                        any = true;
                                    }
                                }
                                if (!any)
                                {
                                    clientMsg(101, "Spelling of name is incorrect.");
                                }
                                return;
                            }
                            if (cmd == "/dig")
                            {
                                session->action(world.charId, world.actIndex, 0x11); // ACTION 0x11 ChocoboDig (mounted)
                                return;
                            }
                            if (cmd == "/quarry" || cmd == "/sprint" || cmd == "/scout")
                            {
                                // Ballista actions 0x15 / 0x16 / 0x17 (LSB does not implement them yet).
                                session->action(world.charId, world.actIndex, cmd == "/quarry" ? 0x15 : cmd == "/sprint" ? 0x16 : 0x17);
                                return;
                            }
                            // Client switches: "on" / "off", else a toggle.
                            if (cmd == "/hidefaith" || cmd == "/hidetrust" || cmd == "/ignorefaith" || cmd == "/ignoretrust" || cmd == "/ignorepet")
                            {
                                bool& flag = cmd.starts_with("/hide") ? hideTrusts : cmd == "/ignorepet" ? ignorePet : ignoreTrusts;
                                flag       = arg == "on" ? true : arg == "off" ? false : !flag;
                                session->note(fmt::format("{}: {}.", cmd.substr(1), flag ? "on" : "off"));
                                return;
                            }
                            if (cmd == "/dismount")
                            {
                                session->action(world.charId, world.actIndex, 0x12); // ACTION 0x12 Dismount
                                return;
                            }
                            if (cmd == "/makelinkpearl" || cmd == "/makeli" || cmd == "/makelinkpearl2" || cmd == "/makeli2")
                            {
                                // GROUP_COMLINK_MAKE {State, LinkshellId 1 / 2}: a pearl from the equipped linkshell.
                                std::vector<std::byte> pkt(4);
                                pkt[0] = std::byte{ 1 };
                                pkt[1] = static_cast<std::byte>(cmd.back() == '2' ? 2 : 1);
                                session->send(0x0C3, std::move(pkt));
                                return;
                            }
                            if (cmd == "/ver")
                            {
                                session->note("ffxi-linux xi-client, client version " + args.clientVer);
                                return;
                            }
                            // /names [on | off] [otherpc] (CODE 0x10079010): on shows the names, off hides them, else toggles;
                            // when that hides them, "otherpc" (the last of the first two arguments) hides only other
                            // players' (0x1047d695 = 4; the name pass 0x100d0440 then keeps ours, NPCs' and monsters').
                            if (cmd == "/names")
                            {
                                const auto first = arg.substr(0, arg.find(' '));
                                if (first == "on")
                                {
                                    namesHidden = 0;
                                    return;
                                }
                                if (first != "off" && namesHidden != 0)
                                {
                                    namesHidden = 0;
                                    return;
                                }
                                const auto sp2  = arg.find(' ');
                                const auto last = sp2 == std::string::npos ? arg : arg.substr(sp2 + 1, arg.find(' ', sp2 + 1) - sp2 - 1);
                                namesHidden     = last == "otherpc" ? 2 : 1;
                                return;
                            }
                            // /help (/h, /?) [command]: the game's usage text (55687); without a command, how /help works.
                            if ((cmd == "/help" || cmd == "/h" || cmd == "/?") && usageText)
                            {
                                const std::string want = arg.empty() ? std::string() : (arg[0] == '/' ? arg : "/" + arg);
                                for (std::size_t i = 1; i < usageText->size(); ++i)
                                {
                                    std::string e(usageText->get(i));
                                    const auto  u = e.find("USAGE: ");
                                    if (u == std::string::npos)
                                    {
                                        continue;
                                    }
                                    const std::string names = e.substr(0, u) + " " + e.substr(u + 7, e.find_first_of(" \x81", u + 7) - u - 7);
                                    auto has = [&](const std::string& n) {
                                        for (std::size_t p = names.find(n); p != std::string::npos; p = names.find(n, p + 1))
                                        {
                                            const auto after = static_cast<unsigned char>(p + n.size() < names.size() ? names[p + n.size()] : ' ');
                                            if (after == ',' || after == ' ' || after < 0x20 || after >= 0x80)
                                            {
                                                return true;
                                            }
                                        }
                                        return false;
                                    };
                                    if (want.empty() ? i != 1 : !has(want))
                                    {
                                        continue;
                                    }
                                    // "USAGE: ..." then the explanation; double-byte spaces separate parts, ">>" breaks lines.
                                    std::string body = e.substr(u);
                                    std::string out;
                                    for (std::size_t k = 0; k < body.size(); ++k)
                                    {
                                        const auto ch = static_cast<unsigned char>(body[k]);
                                        if (ch == 0x07)
                                        {
                                            out += '\n';
                                        }
                                        else if (ch >= 0x80)
                                        {
                                            if (!out.empty() && out.back() != '\n')
                                            {
                                                out += '\n';
                                            }
                                            ++k; // the second byte of the double-byte character
                                        }
                                        else if (body.compare(k, 2, ">>") == 0)
                                        {
                                            out += '\n';
                                            ++k;
                                        }
                                        else
                                        {
                                            out += static_cast<char>(ch);
                                        }
                                    }
                                    std::size_t start = 0;
                                    while (start < out.size())
                                    {
                                        const auto nl = out.find('\n', start);
                                        const auto ln = out.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                                        if (!ln.empty())
                                        {
                                            session->note(ln);
                                        }
                                        if (nl == std::string::npos)
                                        {
                                            break;
                                        }
                                        start = nl + 1;
                                    }
                                    return;
                                }
                                clientMsg(120, "...A command error occurred.");
                                return;
                            }
                            if (cmd == "/follow")
                            {
                                const client::Entity* e = arg.empty() ? target : byName(arg);
                                followId                = e ? e->id : 0;
                                if (!e && !arg.empty())
                                {
                                    clientMsg(101, "Spelling of name is incorrect.");
                                }
                                return;
                            }
                            if (cmd == "/lockon")
                            {
                                lockOn = client::targetLockToggle(lockOn,
                                                                  client::lockable(target)); // command 0x5c, 0x1007a630
                                return;
                            }
                            if (cmd == "/automove")
                            {
                                autorun = !autorun;
                                return;
                            }
                            if (t == "/sort")
                            {
                                session->send(0x03A, std::vector<std::byte>(4)); // ITEM_STACK: merge stacks in the inventory (container 0)
                                return;
                            }
                            if (t == "/logout" || t == "/shutdown")
                            {
                                // As in the game: the server counts down in the log; its 0x00B closes the client.
                                session->requestLogout(t == "/shutdown");
                                return;
                            }
                            // Party commands, as in the game: /pcmd add <name>, /join, /decline, /pcmd leave.
                            if (t.rfind("/pcmd add ", 0) == 0)
                            {
                                const auto who = t.substr(10);
                                for (const auto& e : world.entities)
                                {
                                    if (e.isPc && e.name == who)
                                    {
                                        session->inviteToParty(e.id, e.index);
                                        return;
                                    }
                                }
                                return;
                            }
                            // /tell <name> <message> (or /t): C2S 0x0B6 {u8 3, u8 0, name[15] @2, message @0x11}.
                            if (t.rfind("/tell ", 0) == 0 || t.rfind("/t ", 0) == 0)
                            {
                                const auto rest    = t.substr(t.find(' ') + 1);
                                const auto nameEnd = rest.find(' ');
                                if (nameEnd == std::string::npos || nameEnd == 0 || nameEnd > 15)
                                {
                                    return;
                                }
                                auto msg = expandTokens(rest.substr(nameEnd + 1));
                                if (battleText)
                                {
                                    msg = battleText->encodeAutoTranslate(msg); // "{phrase}" -> FD code
                                }
                                msg.resize(std::min<std::size_t>(msg.size(), 127));
                                std::vector<std::byte> pkt(0x11 + msg.size() + 1);
                                pkt[0] = std::byte{ 3 }; // LSB requires 3 here (the chat kind "tell")
                                std::memcpy(pkt.data() + 2, rest.data(), nameEnd);
                                std::memcpy(pkt.data() + 0x11, msg.data(), msg.size());
                                session->send(0x0B6, std::move(pkt));
                                return;
                            }
                            // Emotes: /wave, /bow ... toward the target, else nobody (C2S 0x05D, mode 0 = text and motion).
                            if (const auto emote = client::emoteId(t.substr(0, t.find(' '))))
                            {
                                std::vector<std::byte> pkt(12);
                                const uint32_t tid  = target ? target->id : world.charId;
                                const uint16_t tidx = target ? target->index : world.actIndex;
                                std::memcpy(pkt.data(), &tid, 4);
                                std::memcpy(pkt.data() + 4, &tidx, 2);
                                pkt[6] = static_cast<std::byte>(*emote);
                                // Subcommands (the game's /help): "text" only, "motion" only, else both (EmoteMode 1 / 2 / 0).
                                const auto mode = t.find(" text") != std::string::npos ? 1 : t.find(" motion") != std::string::npos ? 2 : 0;
                                pkt[7]          = static_cast<std::byte>(mode);
                                // /bell NOTE and /jobemote JOB carry their argument in Param @8 (the Emote List's entries).
                                if (const auto argAt = t.find(' '); argAt != std::string::npos)
                                {
                                    const auto     word  = t.substr(argAt + 1, t.find(' ', argAt + 1) - argAt - 1);
                                    const uint16_t param = client::emoteParam(*emote, word);
                                    std::memcpy(pkt.data() + 8, &param, 2);
                                }
                                session->send(0x05D, std::move(pkt));
                                return;
                            }
                            // /check (or /c): what the target is like (C2S 0x0DD, kind 0); the answer is a battle message.
                            if ((t == "/check" || t == "/c") && target)
                            {
                                std::vector<std::byte> pkt(12);
                                const uint32_t idx = target->index;
                                std::memcpy(pkt.data(), &target->id, 4);
                                std::memcpy(pkt.data() + 4, &idx, 4);
                                session->send(0x0DD, std::move(pkt));
                                return;
                            }
                            if (t == "/join" || t == "/decline")
                            {
                                session->answerPartyInvite(t == "/join");
                                return;
                            }
                            if (t == "/pcmd leave")
                            {
                                session->send(0x06F, std::vector<std::byte>(4)); // GROUP_LEAVE, Kind 0 = party
                                return;
                            }
                            if (!t.empty() && t[0] == '/')
                            {
                                clientMsg(120, "...A command error occurred."); // not a command this client knows
                                return;
                            }
                            session->say(expandTokens(t), kind);
                        }

} // namespace xi::app

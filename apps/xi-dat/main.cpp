// SPDX-License-Identifier: GPL-3.0-or-later
// xi-dat: inspect an FFXI install's DAT files by numeric file id.
//
//   xi-dat [--root DIR] stats
//   xi-dat [--root DIR] se-wav ID OUT.wav   (an .spw sound effect as 16-bit PCM WAV)
//   xi-dat [--root DIR] path ID...
//   xi-dat [--root DIR] head ID [BYTES]
//   xi-dat [--root DIR] chunks ID
//   xi-dat [--root DIR] textures ID OUTDIR   (decodes every texture chunk to PNG)
//   xi-dat [--root DIR] zone ZONEID           (decodes zone geometry, prints totals)
//   xi-dat [--root DIR] weather ZONEID WEATHERID HH:MM

#include "xi/install.h"
#include "client/actor_ground.h"
#include "client/effect_ops.h"
#include "client/game_camera.h"
#include "dat/chunk.h"
#include "dat/sound.h"
#include "dat/effect.h"
#include "dat/dat_index.h"
#include "dat/event.h"
#include "dat/event_runner.h"
#include "dat/messages.h"
#include "dat/motion.h"
#include "dat/character.h"
#include "dat/ui.h"
#include "dat/texture.h"
#include "dat/hd_textures.h"
#include "dat/weather.h"
#include "dat/items.h"
#include "dat/zone.h"
#include "gfx/cloth.h"
#include "gfx/zone_collision.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <tuple>

#include <chrono>
#include <map>
#include <cmath>

#include <filesystem>
#include "xi/bytes.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::string              root;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--root" && i + 1 < argc)
        {
            root = argv[++i];
        }
        else
        {
            args.emplace_back(argv[i]);
        }
    }
    if (root.empty())
    {
        if (const char* env = xi::install::gameRootOrNull()) // FFXI_ROOT, else our imported copy, else ~/.wine
        {
            root = env;
        }
    }
    if (root.empty() || args.empty())
    {
        fmt::print(stderr, "usage: xi-dat [--root DIR | $FFXI_ROOT] stats | path ID... | head ID [BYTES] | chunks ID | textures ID OUTDIR | zone ZONEID | meshes ZONE [X Z R | chunk:NAME] | weather ZONE ID HH:MM | strings ID | ui-layout NAME [FILEID] | dialog FILEID FROM [TO] | ui-sprite SHEET INDEX [FILEID] | event ZONE | event-ops ZONE OP [SUB] | walkcheck ZONE [AGENTS] [STEPS] | materials ZONE [M [N]] | reach ZONE [o]STEPUP... | movetest ZONE X Z TOX TOZ [MAXY] | route ZONE X Z X Z [Y] | se-wav ID OUT.wav | hdtex build|stats [...]\n");
        return 2;
    }

    try
    {
        const xi::dat::DatIndex idx(root);
        const auto&             cmd = args[0];
        if (cmd == "clothbench" && args.size() >= 2)
        {
            // clothbench MODEL [ACTORS] [SECONDS]: the soft-body cost (goal 25): ACTORS copies of an NPC model's
            // cloth stepped at 60 fps for SECONDS while walking round a circle; prints µs per actor per frame.
            const auto id      = static_cast<uint16_t>(std::stoul(args[1]));
            const int  actors  = args.size() >= 3 ? std::stoi(args[2]) : 50;
            const int  seconds = args.size() >= 4 ? std::stoi(args[3]) : 10;
            const auto a       = xi::dat::loadNpcAsset(idx, id);
            if (!a || a->clothParticles == 0)
            {
                fmt::print("model {}: no cloth\n", id);
                return 1;
            }
            const xi::gfx::ClothModel m{ a->cloth, a->skeleton.spheres, static_cast<uint32_t>(a->skeleton.bones.size()), a->clothParticles };
            const glm::mat4           g(glm::vec4(1, 0, 0, 0), glm::vec4(0, -1, 0, 0), glm::vec4(0, 0, -1, 0), glm::vec4(0, 0, 0, 1));
            std::vector<glm::mat4>    bind;
            for (const auto& b : a->skeleton.bindWorld)
            {
                bind.push_back(g * glm::make_mat4(b.data()));
            }
            std::vector<xi::gfx::ClothSim> sims(static_cast<std::size_t>(actors));
            std::vector<glm::mat4>         pal;
            double                         simUs  = 0.0;
            const int                      frames = seconds * 60;
            for (int f = 0; f < frames; ++f)
            {
                for (int k = 0; k < actors; ++k)
                {
                    const float     t = static_cast<float>(f) / 60.0f + static_cast<float>(k);
                    const glm::vec3 at(4.0f * std::cos(t), 0.0f, 4.0f * std::sin(t)); // ~4 yalms/s round a circle
                    const glm::mat4 tr = glm::translate(glm::mat4(1.0f), at);
                    pal.resize(bind.size());
                    for (std::size_t i = 0; i < bind.size(); ++i)
                    {
                        pal[i] = tr * bind[i];
                    }
                    const auto t0 = std::chrono::steady_clock::now();
                    xi::gfx::simulateCloth(m, sims[static_cast<std::size_t>(k)], pal, at, 1.0f / 60.0f);
                    simUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
                }
            }
            fmt::print("model {}: {} particles, {} meshes; {} actors x {} frames: {:.2f} us per actor per frame ({:.3f} ms per frame for all)\n", id,
                       a->clothParticles, a->cloth.size(), actors, frames, simUs / (actors * frames), simUs / frames / 1000.0);
        }
        else if (cmd == "se-wav" && args.size() >= 3)
        {
            const auto w = xi::dat::loadSeWave(root, static_cast<uint32_t>(std::stoul(args[1])));
            if (!w || w->pcm.empty())
            {
                fmt::print(stderr, "se {}: not found or not ADPCM\n", args[1]);
                return 1;
            }
            const uint32_t rate = w->sampleRate ? w->sampleRate : 22050, ch = std::max<uint32_t>(1, w->channels);
            const uint32_t data = static_cast<uint32_t>(w->pcm.size() * 2);
            std::vector<std::byte> out(44 + data);
            auto put32 = [&](std::size_t at, uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
            auto put16 = [&](std::size_t at, uint16_t v) { std::memcpy(out.data() + at, &v, 2); };
            std::memcpy(out.data(), "RIFF", 4), put32(4, 36 + data), std::memcpy(out.data() + 8, "WAVEfmt ", 8);
            put32(16, 16), put16(20, 1), put16(22, static_cast<uint16_t>(ch)), put32(24, rate), put32(28, rate * ch * 2);
            put16(32, static_cast<uint16_t>(ch * 2)), put16(34, 16), std::memcpy(out.data() + 36, "data", 4), put32(40, data);
            std::memcpy(out.data() + 44, w->pcm.data(), data);
            std::ofstream(args[2], std::ios::binary).write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
            fmt::print("se {}: {} Hz, {} ch, {:.3f} s -> {}\n", args[1], rate, ch, static_cast<double>(w->pcm.size() / ch) / rate, args[2]);
        }
        else if (cmd == "stats")
        {
            fmt::print("{} ids, {} present\n", idx.size(), idx.presentCount());
        }
        else if (cmd == "path")
        {
            for (std::size_t i = 1; i < args.size(); ++i)
            {
                const auto id = static_cast<uint32_t>(std::stoul(args[i], nullptr, 0));
                const auto p  = idx.path(id);
                fmt::print("{}\t{}\n", id, p ? p->string() : "(absent)");
            }
        }
        else if (cmd == "head" && args.size() >= 2)
        {
            const auto id    = static_cast<uint32_t>(std::stoul(args[1], nullptr, 0));
            const auto bytes = args.size() >= 3 ? std::stoul(args[2], nullptr, 0) : 256ul;
            const auto data  = idx.read(id);
            fmt::print("{} bytes\n{}", data.size(), xi::hexdump(std::span(data).first(std::min<std::size_t>(bytes, data.size()))));
        }
        else if (cmd == "event-ops" && args.size() >= 3)
        {
            // Every instruction OP (with sub-op SUB) in a zone's event blocks, walked linearly by length, with the
            // event whose entry comes last before it (a static scan: branches a headless run skips are seen too).
            const auto zone = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto op   = static_cast<uint8_t>(std::stoul(args[2], nullptr, 16));
            const int  sub  = args.size() >= 4 ? static_cast<int>(std::stoul(args[3], nullptr, 16)) : -1;
            std::map<uint32_t, std::vector<std::pair<uint16_t, uint16_t>>> blocks; // actor -> (entry, event)
            std::map<uint32_t, xi::dat::EventScript>                          code;
            for (const auto& [actor, ev] : xi::dat::EventScript::entries(idx, zone))
            {
                if (auto s = xi::dat::EventScript::find(idx, zone, actor, ev))
                {
                    blocks[actor].emplace_back(s->entry, ev);
                    code.try_emplace(actor, std::move(*s));
                }
            }
            // event-ops ZONE ACTOR FROM TO: every instruction of that actor's block in [FROM, TO) instead.
            const bool listing = args.size() >= 5 && args[2].starts_with("0x") && std::stoul(args[2], nullptr, 16) > 0xFF;
            const auto from    = listing ? std::stoul(args[3], nullptr, 0) : 0ul;
            const auto to      = listing ? std::stoul(args[4], nullptr, 0) : ~0ul;
            for (auto& [actor, list] : blocks)
            {
                std::sort(list.begin(), list.end());
                const auto& c = code.at(actor).code;
                if (listing && actor != std::stoul(args[2], nullptr, 16))
                {
                    continue;
                }
                for (std::size_t at = 0; at < c.size();)
                {
                    const int len = xi::dat::eventOpLength(c, at);
                    if (listing ? at >= from && at < to : c[at] == op && (sub < 0 || (at + 1 < c.size() && c[at + 1] == sub)))
                    {
                        int ev = -1;
                        for (const auto& [e, id] : list)
                        {
                            ev = e <= at ? id : ev;
                        }
                        const auto n = std::min<std::size_t>(len > 0 ? static_cast<std::size_t>(len) : 8, c.size() - at);
                        fmt::print("zone {} actor {:#x} event {} @{:#x}: {:02x}\n", zone, actor, ev, at, fmt::join(std::span(c).subspan(at, n), " "));
                    }
                    at += len > 0 ? static_cast<std::size_t>(len) : 1;
                }
            }
        }
        else if (cmd == "event" && args.size() >= 2)
        {
            // Runs every event entry of a zone headlessly (Enter at each wait, the first option at each menu, updates
            // answered at once) and counts how they end: a robustness check of dat::EventVm.
            const auto zone = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            int        ended = 0, errors = 0, messages = 0, menus = 0;
            for (const auto& [actor, ev] : xi::dat::EventScript::entries(idx, zone))
            {
                auto script = xi::dat::EventScript::find(idx, zone, actor, ev);
                if (!script)
                {
                    continue;
                }
                xi::dat::EventVm vm(*script, {});
                for (int step = 0; step < 500; ++step)
                {
                    const auto st = vm.run();
                    using S       = xi::dat::EventVm::Stop;
                    if (st.stop == S::Message)
                    {
                        ++messages;
                    }
                    else if (st.stop == S::Menu)
                    {
                        ++menus;
                        vm.choose(0);
                    }
                    else if (st.stop == S::End)
                    {
                        ++ended;
                        break;
                    }
                    else if (st.stop == S::Error)
                    {
                        ++errors;
                        fmt::print("error: actor {:#x} event {}: op {:#04x} sub {} at {}\n", actor, ev, st.message, st.speaker < script->code.size() - 1 ? script->code[st.speaker + 1] : 0, st.speaker);
                        break;
                    }
                }
            }
            fmt::print("zone {}: {} events ended, {} errors; {} messages, {} menus\n", zone, ended, errors, messages, menus);
        }
        else if (cmd == "cutscene-ids" && args.size() >= 2)
        {
            // cutscene-ids ZONE [OWNER]: the event ids the owner's block (default the player's) has an entry for.
            const auto zone  = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto owner = args.size() >= 3 ? static_cast<uint32_t>(std::stoul(args[2], nullptr, 0)) : 0x7FFFFFF0u;
            const auto file  = xi::dat::EventFile::load(idx, zone);
            if (const auto* b = file.block(owner))
            {
                for (const auto id : b->entryId)
                {
                    fmt::print("{}\n", id);
                }
            }
        }
        else if (cmd == "cutscene" && args.size() >= 3)
        {
            // cutscene ZONE EVENT [OWNER]: runs one event's threads headlessly with every actor present, messages closed
            // and menus answered with option 0 at once, shots and fades finishing at once; prints what the script asks for.
            const auto zone  = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto event = static_cast<uint16_t>(std::stoul(args[2], nullptr, 0));
            const auto owner = args.size() >= 4 ? static_cast<uint32_t>(std::stoul(args[3], nullptr, 0)) : 0x7FFFFFF0u;
            struct Host : xi::dat::EventHost
            {
                std::map<uint32_t, xi::dat::EventVec> pos;
                int                                   messages = 0, menus = 0, moves = 0, motions = 0, shots = 0, answered = 0;
                xi::dat::EventRunner*                 runner   = nullptr;
                std::array<uint32_t, 8>               answer{}; // what updates get back (XI_CS_PARAMS)
                static std::string fcc(uint32_t v)
                {
                    std::string n(reinterpret_cast<const char*>(&v), 4);
                    return n;
                }
                uint32_t player() const override { return 0x1000; }
                bool exists(uint32_t) const override { return true; }
                void request(uint32_t id) override { fmt::print("  request {:#x}\n", id); }
                xi::dat::EventVec position(uint32_t id) const override
                {
                    const auto it = pos.find(id);
                    return it != pos.end() ? it->second : xi::dat::EventVec{};
                }
                void place(uint32_t id, const xi::dat::EventVec& p, std::optional<float> yaw, bool) override
                {
                    const bool teleport = std::hypot(p.x - position(id).x, p.z - position(id).z) > 2.0f;
                    pos[id] = p;
                    ++moves;
                    if (teleport)
                    {
                        fmt::print("  place {:#x} at ({:.2f}, {:.2f}, {:.2f}){}\n", id, p.x, p.y, p.z, yaw ? fmt::format(" yaw {:.2f}", *yaw) : "");
                    }
                }
                void face(uint32_t, float) override {}
                void walking(uint32_t, float) override {}
                void hide(uint32_t id, bool h) override { fmt::print("  {} {:#x}\n", h ? "hide" : "show", id); }
                void motion(uint32_t id, uint32_t, uint32_t f, int file, int raceSet) override
                {
                    ++motions;
                    fmt::print("  motion {:#x} {} (file {}, set {})\n", id, fcc(f), file, raceSet);
                }
                bool motionBusy(uint32_t) const override { return false; }
                void idle(uint32_t id, uint32_t f) override { fmt::print("  idle {:#x} {}\n", id, fcc(f)); }
                void lookAt(uint32_t id, uint32_t t, int pt) override { fmt::print("  look {:#x} at {:#x} point {}\n", id, t, pt); }
                void lookAngles(uint32_t id, int p, int y) override { fmt::print("  look {:#x} pitch {} yaw {}\n", id, p, y); }
                void lookStop(uint32_t id) override { fmt::print("  look {:#x} stop\n", id); }
                void costume(uint32_t id, int slot, uint16_t v) override { fmt::print("  costume {:#x} slot {} = {:#x}\n", id, slot, v); }
                void scale(uint32_t id, float k) override { fmt::print("  scale {:#x} {:.2f}\n", id, k); }
                void scheduler(uint32_t file, uint32_t c, uint32_t, uint32_t f) override
                {
                    ++shots;
                    fmt::print("  scheduler {} from {} on {:#x}\n", fcc(f), file, c);
                }
                void stopScheduler(uint32_t, uint32_t, uint32_t) override {}
                bool schedulerRunning(uint32_t, uint32_t, uint32_t) const override { return false; }
                void cameraMode(int m) override { fmt::print("  camera mode {}\n", m); }
                void music(int slot, uint16_t id) override { fmt::print("  music {} = {}\n", slot, id); }
                void sound(uint32_t id) override { fmt::print("  sound {}\n", id); }
                void nameRecord(std::size_t n, const std::string& name, uint32_t v) override { fmt::print("  name record {} = '{}' value {}\n", n, name, v); }
                void message(uint32_t m, uint32_t sp) override
                {
                    ++messages;
                    fmt::print("  message {} ({:#x})\n", m, sp);
                    runner->closeMessage();
                }
                void menu(uint32_t m, uint32_t, uint32_t def, uint32_t) override
                {
                    ++menus;
                    fmt::print("  menu {} default {}\n", m, def);
                }
                void update(uint32_t r) override
                {
                    fmt::print("  update {}\n", r);
                    runner->updated(answer);
                }
                void chocobo(uint32_t id, const std::array<uint16_t, 6>& l) override { fmt::print("  chocobo {:#x} {}\n", id, l); }
                void animation(uint32_t id, uint8_t v) override { fmt::print("  animation {:#x} = {}\n", id, v); }
                void fade(uint32_t id, int alpha, float t) override { fmt::print("  fade {:#x} to {} over {}\n", id, alpha, t); }
                void emote(uint32_t id, uint32_t, int e, int m) override { fmt::print("  emote {:#x} {} mode {}\n", id, e, m); }
                void environment(int h, int w) override { fmt::print("  environment hour {} weather {}\n", h, w); }
                void restoreEnvironment() override { fmt::print("  environment restored\n"); }
                void volume(uint32_t mask, float level, float t) override { fmt::print("  volume {:#x} = {:.2f} over {}\n", mask, level, t); }
                void subMap(uint16_t n) override { fmt::print("  sub-map {}\n", n); }
                void eventTarget(const std::string& name, int gauge) override { fmt::print("  target window '{}' gauge {}\n", name, gauge); }
                void eventTimer(int which, int ticks) override { fmt::print("  {} timer {}\n", which ? "event" : "cast", ticks); }
                void mapView(float x, float y, float s, float t, int f) override { fmt::print("  map view {:.3f} {:.3f} {:.3f} over {:.3f} flags {:#x}\n", x, y, s, t, f); }
                void positionUpdate(const xi::dat::EventVec& p, float yaw) override
                {
                    fmt::print("  position update ({:.2f}, {:.2f}, {:.2f}) yaw {:.2f}\n", p.x, p.y, p.z, yaw);
                    runner->updated(answer);
                }
                void mapShow(int zone, int map) override { fmt::print("  map {} / {}\n", zone, map); }
                void mapMarker(int zone, int slot, float x, float z, const std::string& label) override
                {
                    fmt::print("  map {} marker {} ({:.1f}, {:.1f}) '{}'\n", zone, slot, x, z, label);
                }
                void mapClose() override { fmt::print("  map closed\n"); }
            } host;
            // XI_CS_PARAMS=a,b,...: the event's parameters; XI_CS_CHOICES=a,b,...: the options picked at its menus in turn
            // (then 0).
            const auto list = [](const char* env) {
                std::vector<uint32_t> v;
                for (std::string s = env ? env : ""; !s.empty();)
                {
                    const auto comma = s.find(',');
                    v.push_back(static_cast<uint32_t>(std::stoul(s.substr(0, comma), nullptr, 0)));
                    s = comma == std::string::npos ? "" : s.substr(comma + 1);
                }
                return v;
            };
            std::array<uint32_t, 8> params{};
            const auto              given = list(std::getenv("XI_CS_PARAMS"));
            std::copy_n(given.begin(), std::min<std::size_t>(given.size(), 8), params.begin());
            const auto choices = list(std::getenv("XI_CS_CHOICES"));
            host.answer        = params;
            auto       file    = std::make_shared<const xi::dat::EventFile>(xi::dat::EventFile::load(idx, zone));
            xi::dat::EventRunner runner(file, owner, event, params, host);
            if (std::getenv("XI_EVENT_TRACE"))
            {
                spdlog::set_level(spdlog::level::debug);
                runner.setTrace(true);
            }
            host.runner = &runner;
            if (!runner.valid())
            {
                fmt::print("no entry for event {} in block {:#x}\n", event, owner);
                return 1;
            }
            int frame = 0;
            for (; frame < 60 * 600 && !runner.finished(); ++frame)
            {
                runner.tick(1.0f);
                runner.closeMessage(); // messages close at once (a menu is answered below)
                if (host.menus > host.answered)
                {
                    const auto k = static_cast<std::size_t>(host.answered++);
                    runner.choose(k < choices.size() ? static_cast<int>(choices[k]) : 0);
                }
            }
            fmt::print("event {}: {} after {:.1f} s; {} messages, {} menus, {} moves, {} motions, {} schedulers; result {:#x}\n", event,
                       runner.finished() ? "ended" : "still running", frame / 60.0, host.messages, host.menus, host.moves, host.motions, host.shots, runner.result());
            for (const auto& [op, n] : runner.skipped())
            {
                fmt::print("  skipped op {:x}: {}\n", op, n);
            }
            for (const auto& [op, n] : runner.stateOnly())
            {
                fmt::print("  state-only op {:x}: {}\n", op, n);
            }
        }
        else if (cmd == "ui-bbox")
        {
            // Every sprite of every sheet: sheet, index, bounding box (minX minY maxX maxY), quad count, textures.
            const uint32_t file  = args.size() >= 2 ? static_cast<uint32_t>(std::stoul(args[1], nullptr, 0)) : xi::dat::kUiMenuFile;
            const uint32_t ids[] = { file };
            const auto     res   = xi::dat::UiResources::load(idx, ids);
            for (const auto& [name, sheet] : res.sheets)
            {
                for (std::size_t i = 0; i < sheet.sprites.size(); ++i)
                {
                    int         x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
                    std::string tex;
                    for (const auto& q : sheet.sprites[i])
                    {
                        for (int k = 0; k < 4; ++k)
                        {
                            x0 = std::min<int>(x0, q.xy[2 * k]), x1 = std::max<int>(x1, q.xy[2 * k]);
                            y0 = std::min<int>(y0, q.xy[2 * k + 1]), y1 = std::max<int>(y1, q.xy[2 * k + 1]);
                        }
                        if (tex.find(q.texture) == std::string::npos)
                        {
                            tex += (tex.empty() ? "" : ",") + q.texture;
                        }
                    }
                    fmt::print("{}\t{}\t{} {} {} {}\t{}\t{}\n", name, i, x0, y0, x1, y1, sheet.sprites[i].size(), tex);
                }
            }
        }
        else if (cmd == "ui-windows")
        {
            // Every layout's window record: name, x, y, w, h, anchor, first sub (sheet / shape).
            const uint32_t file  = args.size() >= 2 ? static_cast<uint32_t>(std::stoul(args[1], nullptr, 0)) : xi::dat::kUiMenuFile;
            const uint32_t ids[] = { file };
            const auto     res   = xi::dat::UiResources::load(idx, ids);
            for (const auto& [name, l] : res.layouts)
            {
                const auto& w = l.window;
                fmt::print("{}\t{} {} {}x{} a{}\t{}\n", name, w.x, w.y, w.w, w.h, w.anchor,
                           w.subs.empty() ? std::string() : fmt::format("{} {}", w.subs[0].sheet, w.subs[0].shape));
            }
        }
        else if (cmd == "ui-labels")
        {
            // Every sprite with "menu menu2fon" quads (the small title letters: "Target" is T a r g e t), one line each:
            // sheet, index, then the letter quads' texel corners u,v in drawing order.
            const uint32_t file  = args.size() >= 2 ? static_cast<uint32_t>(std::stoul(args[1], nullptr, 0)) : xi::dat::kUiMenuFile;
            const uint32_t ids[] = { file };
            const auto     res   = xi::dat::UiResources::load(idx, ids);
            for (const auto& [name, sheet] : res.sheets)
            {
                for (std::size_t i = 0; i < sheet.sprites.size(); ++i)
                {
                    std::string uv;
                    for (const auto& q : sheet.sprites[i])
                    {
                        if (q.texture == "menu menu2fon" && q.w <= 12)
                        {
                            uv += fmt::format(" {},{}", q.u, q.v);
                        }
                    }
                    if (!uv.empty())
                    {
                        fmt::print("{}\t{}\t{}\n", name, i, uv);
                    }
                }
            }
        }
        else if (cmd == "ui-sprite" && args.size() >= 3)
        {
            // One interface sprite's quads: rect, texel rect, flags, blend {enable, A, B, C}, corner colours, texture.
            const uint32_t file  = args.size() >= 4 ? static_cast<uint32_t>(std::stoul(args[3], nullptr, 0)) : xi::dat::kUiMenuFile;
            const uint32_t ids[] = { file };
            const auto     res   = xi::dat::UiResources::load(idx, ids);
            const auto*    sp    = res.sprite(args[1], std::stoul(args[2], nullptr, 0));
            if (!sp)
            {
                fmt::print(stderr, "no sprite {} {}\n", args[1], args[2]);
                return 1;
            }
            for (const auto& q : *sp)
            {
                fmt::print("xy [{}] uv {},{} {}x{} flags {} blend [{}] rgba", fmt::join(q.xy, " "), q.u, q.v, q.w, q.h, q.flags, fmt::join(q.blend, " "));
                for (const auto& c : q.rgba)
                {
                    fmt::print(" {:02x}{:02x}{:02x}{:02x}", c[0], c[1], c[2], c[3]);
                }
                fmt::print(" tex '{}'\n", q.texture);
            }
        }
        else if (cmd == "dialog" && args.size() >= 3)
        {
            // Dialog-format messages (zone dialog 6420 + zone, ...), formatted with empty arguments; hex of the raw bytes.
            const auto table = xi::dat::DialogTable::parse(idx.read(static_cast<uint32_t>(std::stoul(args[1], nullptr, 0))));
            const auto from  = std::stoul(args[2], nullptr, 0);
            const auto to    = args.size() >= 4 ? std::stoul(args[3], nullptr, 0) : from;
            for (auto i = from; i <= to && i < table.size(); ++i)
            {
                const auto raw = table.raw(i);
                fmt::print("{}\t{}\n", i, fmt::join(xi::dat::formatDialog(raw, {}), " / "));
                fmt::print("\t{}\n", xi::hexdump(std::as_bytes(std::span(raw.data(), std::min<std::size_t>(raw.size(), 48)))));
            }
        }
        else if (cmd == "ui-layout" && args.size() >= 2)
        {
            // A menu layout's records ("menu fulllog"; the file defaults to the English menu file).
            const uint32_t file = args.size() >= 3 ? static_cast<uint32_t>(std::stoul(args[2], nullptr, 0)) : xi::dat::kUiMenuFile;
            const uint32_t ids[] = { file };
            const auto     res   = xi::dat::UiResources::load(idx, ids);
            const auto     it    = res.layouts.find(args[1]);
            if (it == res.layouts.end())
            {
                fmt::print(stderr, "no layout '{}' in file {}\n", args[1], file);
                return 1;
            }
            auto row = [](const char* what, const xi::dat::UiRecord& r) {
                fmt::print("{} id {} xy {},{} wh {}x{} frame {},{} cursor {},{} anchor {} nav [{}] text {} {} subs", what, r.id, r.x, r.y, r.w, r.h,
                           r.frameX, r.frameY, r.cursorX, r.cursorY, r.anchor, fmt::join(r.nav, " "), r.text1, r.text2);
                for (const auto& sb : r.subs)
                {
                    fmt::print(" {{{} {} {}}}", sb.type, sb.shape, sb.sheet);
                }
                fmt::print("\n");
            };
            row("window", it->second.window);
            for (const auto& e : it->second.elements)
            {
                row("  element", e);
            }
        }
        else if (cmd == "strings" && args.size() >= 2)
        {
            // A d_msg string table, one entry per line: index, then its fields separated by " | ".
            const auto data = idx.read(static_cast<uint32_t>(std::stoul(args[1], nullptr, 0)));
            if (data.size() >= 8 && std::memcmp(data.data(), "XISTRING", 8) == 0)
            {
                // An XISTRING table (string table 10 = file 50, ...): one string per entry.
                const auto xs = xi::dat::XiStringTable::parse(data);
                for (std::size_t i = 0; i < xs.size(); ++i)
                {
                    fmt::print("{}\t{}\n", i, xs.get(i));
                }
                return 0;
            }
            const auto table = xi::dat::StringTable::parse(data);
            for (std::size_t i = 0; i < table.size(); ++i)
            {
                fmt::print("{}", i);
                for (std::size_t f = 0; f < std::max<std::size_t>(1, table.fields(i)); ++f)
                {
                    if (table.get(i, f).empty() && table.number(i, f) != 0)
                    {
                        fmt::print("\t#{}", table.number(i, f));
                    }
                    else
                    {
                        fmt::print("\t{}", table.get(i, f));
                    }
                }
                fmt::print("\n");
            }
        }
        else if (cmd == "fxsched" && args.size() >= 3)
        {
            // One effect file's schedulers (or generators, "gen:NAME") named NAME ("*" = all): every record as
            // op / length / delay / duration and its dwords (effect-format §S.1, §G.3).
            const auto           id = static_cast<uint32_t>(std::stoul(args[1], nullptr, 0));
            xi::dat::EffectFile  f(idx, id);
            const bool           gen  = args[2].starts_with("gen:");
            const std::string    want = gen ? args[2].substr(4) : args[2];
            for (const auto& r : f.resources())
            {
                if ((r.type != (gen ? xi::dat::FxType::Generator : xi::dat::FxType::Scheduler)) || (want != "*" && xi::dat::fxNameString(r.name) != want))
                {
                    continue;
                }
                const auto p = r.payload;
                fmt::print("== {} {} ({} bytes)\n", gen ? "gen" : "sched", xi::dat::fxNameString(r.name), p.size());
                std::vector<std::pair<std::string, std::size_t>> lists;
                if (gen)
                {
                    for (std::size_t k = 0; k < 4; ++k)
                    {
                        const auto off = xi::load<uint32_t>(p, 0x70 + 4 * k);
                        lists.emplace_back(fmt::format("L{}", k), off >= 0x10 ? off - 0x10 : SIZE_MAX);
                    }
                }
                else
                {
                    const auto start = xi::load<uint32_t>(p, 0x14);
                    lists.emplace_back("", start >= 0x10 ? start - 0x10 : 0x40);
                }
                for (const auto& [label, start] : lists)
                {
                    for (std::size_t at = start; start != SIZE_MAX && at + 4 <= p.size();)
                    {
                        const auto w   = xi::load<uint32_t>(p, at);
                        const auto len = (w >> 8) & 0x1F;
                        if ((w & 0xFF) == 0 || len == 0)
                        {
                            break;
                        }
                        std::string words;
                        for (std::size_t k = gen ? 1 : 2; k < len && at + 4 * k + 4 <= p.size(); ++k)
                        {
                            const auto v = xi::load<uint32_t>(p, at + 4 * k);
                            const bool printable = std::ranges::all_of(std::array{ v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24 },
                                                                       [](uint32_t c) { return (c >= 0x30 && c < 0x7B) || c == 0x20; });
                            words += printable ? fmt::format(" '{}'", xi::dat::fxNameString(v)) : fmt::format(" {:08x}", v);
                        }
                        if (gen)
                        {
                            fmt::print("  {} {:02x} len {:2} slot {:2}:{}\n", label, w & 0xFF, len, (w >> 13) & 0x3F, words);
                        }
                        else
                        {
                            fmt::print("  {:02x} len {:2} delay {:4} dur {:4}:{}\n", w & 0xFF, len, xi::load<int16_t>(p, at + 4), xi::load<int16_t>(p, at + 6), words);
                        }
                        at += 4 * len;
                    }
                }
            }
        }
        else if (cmd == "fxops" && args.size() >= 2)
        {
            // Effect bytecode survey (effect-format §S.3, §G.9-§G.11): every op of every scheduler and generator list in
            // the given files (ids or FROM-TO ranges), with xi-client's coverage (client/effect_ops.h). Unhandled first.
            using xi::client::FxList;
            using xi::client::FxOpStatus;
            struct Count
            {
                std::size_t        records = 0;
                std::set<uint32_t> files;
                std::string        example;
            };
            std::map<std::pair<FxList, uint8_t>, Count> counts;
            std::size_t                                 nFiles = 0;
            auto walk = [&](std::span<const std::byte> p, std::size_t at, FxList l, uint32_t file, xi::dat::FxName name) {
                for (int guard = 0; at + 4 <= p.size() && guard < 4096; ++guard)
                {
                    const auto w   = xi::load<uint32_t>(p, at);
                    const auto op  = w & 0xFF;
                    const auto len = (w >> 8) & 0x1F;
                    if (op == 0 || len == 0)
                    {
                        break;
                    }
                    auto& c = counts[{ l, static_cast<uint8_t>(op) }];
                    ++c.records;
                    c.files.insert(file);
                    if (c.example.empty())
                    {
                        c.example = fmt::format("{}:{}", file, xi::dat::fxNameString(name));
                    }
                    at += 4 * len;
                }
            };
            for (std::size_t a = 1; a < args.size(); ++a)
            {
                const auto dash = args[a].find('-');
                const auto lo   = static_cast<uint32_t>(std::stoul(args[a].substr(0, dash), nullptr, 0));
                const auto hi   = dash == std::string::npos ? lo : static_cast<uint32_t>(std::stoul(args[a].substr(dash + 1), nullptr, 0));
                for (uint32_t id = lo; id <= hi; ++id)
                {
                    std::unique_ptr<xi::dat::EffectFile> f;
                    try
                    {
                        f = std::make_unique<xi::dat::EffectFile>(idx, id);
                    }
                    catch (const std::exception&)
                    {
                        continue;
                    }
                    ++nFiles;
                    for (const auto& r : f->resources())
                    {
                        const auto p = r.payload;
                        if (r.type == xi::dat::FxType::Scheduler && p.size() >= 0x18)
                        {
                            const auto start = xi::load<uint32_t>(p, 0x14);
                            walk(p, start >= 0x10 ? start - 0x10 : 0x40, FxList::Scheduler, id, r.name);
                        }
                        else if (r.type == xi::dat::FxType::Generator && p.size() >= 0x80)
                        {
                            for (std::size_t k = 0; k < 4; ++k)
                            {
                                const auto off = xi::load<uint32_t>(p, 0x70 + 4 * k);
                                if (off >= 0x10)
                                {
                                    walk(p, off - 0x10, static_cast<FxList>(k + 1), id, r.name);
                                }
                            }
                        }
                    }
                }
            }
            std::vector<std::pair<std::pair<FxList, uint8_t>, const Count*>> rows;
            for (const auto& [k, c] : counts)
            {
                rows.emplace_back(k, &c);
            }
            std::ranges::sort(rows, [](const auto& a, const auto& b) {
                const auto sa = xi::client::fxOpStatus(a.first.first, a.first.second), sb = xi::client::fxOpStatus(b.first.first, b.first.second);
                return sa != sb ? sa > sb : a.second->files.size() > b.second->files.size();
            });
            constexpr const char* kStatus[] = { "handled", "game no-op", "no visual", "UNHANDLED" };
            std::size_t           unhandled = 0;
            for (const auto& [k, c] : rows)
            {
                const auto st = xi::client::fxOpStatus(k.first, k.second);
                unhandled += st == FxOpStatus::Unhandled ? c->records : 0;
                fmt::print("{:<5} {:#04x} {:<10} {:6} records {:5} files  e.g. {}\n", xi::client::fxListName(k.first), k.second, kStatus[static_cast<int>(st)], c->records,
                           c->files.size(), c->example);
            }
            fmt::print("{} effect files, {} unhandled records\n", nFiles, unhandled);
        }
        else if (cmd == "chunks" && args.size() >= 2)
        {
            const auto                    id   = static_cast<uint32_t>(std::stoul(args[1], nullptr, 0));
            const auto                    data = idx.read(id);
            std::vector<xi::dat::Chunk>   chunks;
            const bool                    ok    = xi::dat::parseChunks(data, chunks);
            int                           depth = 0;
            for (const auto& c : chunks)
            {
                if (c.type == 0 && depth > 0)
                {
                    --depth;
                }
                fmt::print("{:08x} {:>{}}{:<4} type {:3} ({:#04x}) {:>9} bytes\n", c.offset, "", depth * 2, c.name, c.type, c.type, c.payload.size());
                if (c.type == 1)
                {
                    ++depth;
                }
            }
            fmt::print("{} chunks, {}\n", chunks.size(), ok ? "walks to EOF" : "NOT a clean chunk file");
        }
        else if (cmd == "hdtex" && args.size() >= 2)
        {
            // hdtex build [--zones A,B|--all] [--files ID,...] [--pending] [--scale 4|2|1] [--max N] [--model NAME]
            //             [--no-maps] [--force] [--dir DIR] [--upscaler PATH]   (goal 41, docs/enhanced-graphics.md)
            // hdtex stats [--dir DIR]
            namespace hd = xi::dat::hd;
            hd::BuildOptions opt;
            opt.dir      = hd::defaultDir();
            opt.upscaler = opt.dir / "tools" / "realesrgan" / "realesrgan-ncnn-vulkan";
            std::vector<uint16_t> zones;
            std::vector<uint32_t> files;
            bool                  all = false, pending = false;
            auto                  list = [](const std::string& v) {
                std::vector<uint32_t> out;
                std::size_t           p = 0;
                while (p < v.size())
                {
                    const auto q = v.find(',', p);
                    out.push_back(static_cast<uint32_t>(std::stoul(v.substr(p, q - p), nullptr, 0)));
                    p = q == std::string::npos ? v.size() : q + 1;
                }
                return out;
            };
            for (std::size_t i = 2; i < args.size(); ++i)
            {
                const auto& a    = args[i];
                auto        next = [&]() -> const std::string& { return args.at(++i); };
                if (a == "--zones") for (const auto z : list(next())) zones.push_back(static_cast<uint16_t>(z));
                else if (a == "--files") files = list(next());
                else if (a == "--all") all = true;
                else if (a == "--pending") pending = true;
                else if (a == "--scale") opt.scale = static_cast<uint32_t>(std::stoul(next()));
                else if (a == "--max") opt.maxSize = static_cast<uint32_t>(std::stoul(next()));
                else if (a == "--model") opt.model = next();
                else if (a == "--no-maps") opt.maps = false;
                else if (a == "--force") opt.force = true;
                else if (a == "--dir") opt.dir = next();
                else if (a == "--upscaler") opt.upscaler = next();
            }
            if (args[1] == "stats")
            {
                std::size_t n = 0, maps = 0;
                uintmax_t   bytes = 0;
                if (std::filesystem::exists(opt.dir / "tex"))
                {
                    for (const auto& e : std::filesystem::recursive_directory_iterator(opt.dir / "tex"))
                    {
                        if (e.is_regular_file())
                        {
                            const auto stem = e.path().stem().string();
                            (stem.ends_with("_n") || stem.ends_with("_r") ? maps : n)++;
                            bytes += e.file_size();
                        }
                    }
                }
                fmt::print("{}: {} textures, {} maps, {:.1f} MB\n", opt.dir.string(), n, maps, static_cast<double>(bytes) / 1e6);
                return 0;
            }
            if (!std::filesystem::exists(opt.upscaler))
            {
                fmt::print(stderr, "hdtex: no upscaler at {} (docs/enhanced-graphics.md: install realesrgan-ncnn-vulkan, or --upscaler PATH)\n", opt.upscaler.string());
                return 1;
            }
            std::vector<std::pair<std::string, hd::Image>> texs;
            std::set<std::string>                          seen;
            auto addFile = [&](const std::vector<std::byte>& bytes) {
                std::vector<xi::dat::Chunk> chunks;
                xi::dat::parseChunks(bytes, chunks);
                for (const auto& c : chunks)
                {
                    if (c.type != 0x20 && c.type != 0x1B)
                    {
                        continue;
                    }
                    try
                    {
                        const auto t = xi::dat::parseTexture(c.payload);
                        if (auto k = hd::key(t); seen.insert(k).second)
                        {
                            texs.emplace_back(std::move(k), hd::Image{ t.width, t.height, xi::dat::toRgba(t) });
                        }
                    }
                    catch (const std::exception&)
                    {
                    }
                }
            };
            if (all)
            {
                for (uint16_t z = 0; z < 300; ++z)
                {
                    zones.push_back(z);
                }
            }
            for (const auto z : zones)
            {
                const auto fid = xi::dat::zoneModelFileId(xi::dat::zoneModelId(z));
                if (idx.path(fid))
                {
                    addFile(idx.read(fid));
                }
            }
            for (const auto f : files)
            {
                if (idx.path(f))
                {
                    addFile(idx.read(f));
                }
            }
            std::vector<std::filesystem::path> pendingFiles;
            if (pending && std::filesystem::exists(opt.dir / "pending"))
            {
                for (const auto& e : std::filesystem::directory_iterator(opt.dir / "pending"))
                {
                    if (auto img = hd::readPng(e.path()); img && seen.insert(e.path().stem().string()).second)
                    {
                        texs.emplace_back(e.path().stem().string(), std::move(*img));
                        pendingFiles.push_back(e.path());
                    }
                }
            }
            const auto t0 = std::chrono::steady_clock::now();
            const auto st = hd::build(texs, opt);
            const auto s  = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            for (const auto& p : pendingFiles)
            {
                if (std::filesystem::exists(hd::entryPath(opt.dir, p.stem().string())))
                {
                    std::filesystem::remove(p);
                }
            }
            fmt::print("hdtex: {} textures: {} built, {} already cached, {} too small, {} failed; {:.1f} MB written in {:.1f} s\n", texs.size(), st.built,
                       st.skipped, st.tiny, st.failed, static_cast<double>(st.bytes) / 1e6, s);
            return st.failed ? 1 : 0;
        }
        else if (cmd == "textures" && args.size() >= 3)
        {
            const auto                  id   = static_cast<uint32_t>(std::stoul(args[1], nullptr, 0));
            const auto                  data = idx.read(id);
            std::vector<xi::dat::Chunk> chunks;
            xi::dat::parseChunks(data, chunks);
            std::filesystem::create_directories(args[2]);
            int ok = 0, failed = 0;
            for (const auto& c : chunks)
            {
                if (c.type != 0x20 && c.type != 0x1B)
                {
                    continue;
                }
                try
                {
                    const auto tex  = xi::dat::parseTexture(c.payload);
                    const auto rgba = xi::dat::toRgba(tex);
                    std::string file = tex.name;
                    std::erase(file, ' ');
                    std::replace(file.begin(), file.end(), '/', '_');
                    const auto out = fmt::format("{}/{:08x}_{}.png", args[2], c.offset, file);
                    xi::dat::writePng(out, tex.width, tex.height, rgba);
                    ++ok;
                }
                catch (const std::exception& e)
                {
                    fmt::print(stderr, "{:08x} {}: {}\n", c.offset, c.name, e.what());
                    ++failed;
                }
            }
            fmt::print("{} textures written, {} failed\n", ok, failed);
        }
        else if (cmd == "vcstats" && args.size() >= 2)
        {
            // Zone vertex colours (the D3D material the lighting multiplies; 0x80 = 1.0): how they spread, how much they
            // vary inside a piece, and whether they follow the normal (baked light) or not (a tint). --pathtrace's
            // albedo policy (docs/enhanced-graphics.md).
            const auto keys = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            for (std::size_t a = 1; a < args.size(); ++a)
            {
                const auto zoneId = static_cast<uint16_t>(std::stoul(args[a], nullptr, 0));
                const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
                std::array<std::size_t, 8> hist{};
                double      sum = 0, sumUp = 0, sumSide = 0, chroma = 0, inPiece = 0;
                std::size_t n = 0, nUp = 0, nSide = 0, pieces = 0;
                for (const auto& [name, m] : zone.models)
                {
                    for (const auto& pc : m.pieces)
                    {
                        if (pc.vertices.empty())
                        {
                            continue;
                        }
                        double pSum = 0, pSq = 0;
                        for (const auto& v : pc.vertices)
                        {
                            const double b = (v.color & 0xFF) / 128.0, g = ((v.color >> 8) & 0xFF) / 128.0, r = ((v.color >> 16) & 0xFF) / 128.0;
                            const double l = 0.299 * r + 0.587 * g + 0.114 * b;
                            ++hist[std::min<std::size_t>(7, static_cast<std::size_t>(l * 4.0))];
                            sum += l;
                            pSum += l;
                            pSq += l * l;
                            chroma += std::max({ r, g, b }) - std::min({ r, g, b });
                            ++n;
                            // The game's +Y is down: a floor's normal points to -Y.
                            if (v.normal[1] < -0.7f)
                            {
                                sumUp += l, ++nUp;
                            }
                            else if (std::abs(v.normal[1]) < 0.3f)
                            {
                                sumSide += l, ++nSide;
                            }
                        }
                        const double k = static_cast<double>(pc.vertices.size());
                        inPiece += std::sqrt(std::max(0.0, pSq / k - (pSum / k) * (pSum / k)));
                        ++pieces;
                    }
                }
                const auto per = [](double s, std::size_t k) { return k ? s / static_cast<double>(k) : 0.0; };
                fmt::print("zone {}: {} vertices; luma mean {:.3f} (1 = 0x80), floors {:.3f}, walls {:.3f}; chroma {:.3f}; spread in a piece {:.3f}\n", zoneId, n,
                           per(sum, n), per(sumUp, nUp), per(sumSide, nSide), per(chroma, n), per(inPiece, pieces));
                fmt::print("  luma histogram (x0.25 bins): ");
                for (const auto h : hist)
                {
                    fmt::print("{:.1f}% ", n ? 100.0 * static_cast<double>(h) / static_cast<double>(n) : 0.0);
                }
                fmt::print("\n");
            }
        }
        else if (cmd == "zone" && args.size() >= 2)
        {
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto t0     = std::chrono::steady_clock::now();
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto t1     = std::chrono::steady_clock::now();
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            const auto t2     = std::chrono::steady_clock::now();
            std::size_t verts = 0, tris = 0, unresolved = 0;
            for (const auto& pl : zone.placements)
            {
                const auto it = zone.models.find(pl.name);
                if (it == zone.models.end())
                {
                    ++unresolved;
                    continue;
                }
                for (const auto& pc : it->second.pieces)
                {
                    verts += pc.vertices.size();
                    tris += pc.triangles.size() / 3;
                }
            }
            auto ms = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
            fmt::print("zone {} (file {}): {} models, {} placements ({} unresolved), placed: {} vertices, {} triangles\n", zoneId,
                       xi::dat::zoneModelFileId(xi::dat::zoneModelId(zoneId)), zone.models.size(), zone.placements.size(), unresolved, verts, tris);
            fmt::print("key tables {} ms, decode+parse {} ms\n", ms(t0, t1), ms(t1, t2));
            {
                std::size_t walls = 0, floors = 0, barriers = 0;
                for (const auto& t : zone.collision)
                {
                    walls += std::abs(t.normal[1]) < 0.2f;
                    floors += t.normal[1] < -0.5f;
                    barriers += t.barrier;
                }
                fmt::print("collision: {} triangles ({} walls, {} floors, {} barrier)\n", zone.collision.size(), walls, floors, barriers);
            }
            for (const auto& l : zone.zoneLines)
            {
                fmt::print("zone line {} -> {}  at ({:.2f}, {:.2f}, {:.2f}) yaw {:.3f} size ({:.1f}, {:.1f}, {:.1f})\n", l.name(),
                           std::string(reinterpret_cast<const char*>(&l.arrival), 4), l.pos[0], l.pos[1], l.pos[2], l.yaw, l.size[0], l.size[1], l.size[2]);
            }
        }
        else if (cmd == "meshes" && args.size() >= 2)
        {
            // Zone meshes and their render states (docs/dat-format.md §6): the placements within R of (X, Z) (all when no
            // position is given) and the Mmbs no placement names (drawn by zone effects), each piece with its texture,
            // flag bits, vertex alpha range and the texture's alpha range.
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const bool near   = args.size() >= 5;
            const float px = near ? std::stof(args[2]) : 0, pz = near ? std::stof(args[3]) : 0, r = near ? std::stof(args[4]) : 0;
            const auto keys = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            std::unordered_map<std::string, std::pair<int, int>> texAlpha; // name -> min, max alpha
            std::unordered_map<std::string, std::string>         texFormat;
            {
                std::vector<xi::dat::Chunk> chunks;
                xi::dat::parseChunks(zone.file, chunks);
                for (const auto& c : chunks)
                {
                    if (c.type != 0x20 && c.type != 0x1B)
                    {
                        continue;
                    }
                    try
                    {
                        const auto tex  = xi::dat::parseTexture(c.payload);
                        const auto rgba = xi::dat::toRgba(tex);
                        int        lo = 255, hi = 0;
                        for (std::size_t i = 3; i < rgba.size(); i += 4)
                        {
                            lo = std::min<int>(lo, rgba[i]);
                            hi = std::max<int>(hi, rgba[i]);
                        }
                        texAlpha.try_emplace(tex.name, lo, hi);
                        static const char* fmts[] = { "DXT1", "DXT3", "DXT5", "BGRA" };
                        texFormat.try_emplace(tex.name, fmts[static_cast<int>(tex.format)]);
                    }
                    catch (const std::exception&)
                    {
                    }
                }
            }
            auto printModel = [&](const std::string& name)
            {
                const auto it = zone.models.find(name);
                if (it == zone.models.end())
                {
                    fmt::print("    (no mesh {})\n", name);
                    return;
                }
                for (const auto& pc : it->second.pieces)
                {
                    int   lo = 255, hi = 0;
                    float bmin[3] = { 1e9f, 1e9f, 1e9f }, bmax[3] = { -1e9f, -1e9f, -1e9f };
                    for (const auto& v : pc.vertices)
                    {
                        lo = std::min<int>(lo, static_cast<int>(v.color >> 24));
                        hi = std::max<int>(hi, static_cast<int>(v.color >> 24));
                        for (int k = 0; k < 3; ++k)
                        {
                            bmin[k] = std::min(bmin[k], v.pos[k]);
                            bmax[k] = std::max(bmax[k], v.pos[k]);
                        }
                    }
                    const auto ta = texAlpha.find(pc.texture);
                    const auto tf = texFormat.find(pc.texture);
                    fmt::print("    piece '{}' flags {:x} ({}{}) verts {} vtxA {:#x}..{:#x} tex {} texA {}\n", pc.texture, pc.flags,
                               (pc.flags & 0x8) ? "blend" : "opaque", (pc.flags & 0x2) ? ",2-sided" : "", pc.vertices.size(), lo, hi,
                               tf != texFormat.end() ? tf->second : "?",
                               ta != texAlpha.end() ? fmt::format("{:#x}..{:#x}", ta->second.first, ta->second.second) : "?");
                    fmt::print("      bounds ({:.1f}, {:.1f}, {:.1f})..({:.1f}, {:.1f}, {:.1f}) tris {}\n", bmin[0], bmin[1], bmin[2], bmax[0], bmax[1],
                               bmax[2], pc.triangles.size() / 3);
                }
            };
            if (args.size() >= 3 && args[2].starts_with("chunk:"))
            {
                const auto want = args[2].substr(6);
                for (const auto& [chunk, modelName] : zone.modelByChunk)
                {
                    if (chunk == want)
                    {
                        fmt::print("chunk '{}' model '{}'\n", chunk, modelName);
                        printModel(modelName);
                    }
                }
                return 0;
            }
            std::unordered_set<std::string> placed;
            for (const auto& pl : zone.placements)
            {
                placed.insert(pl.name);
                const float dx = pl.pos[0] - px, dz = pl.pos[2] - pz;
                if (near && dx * dx + dz * dz > r * r)
                {
                    continue;
                }
                fmt::print("placement '{}' at ({:.1f}, {:.1f}, {:.1f}) lod d {:.0f}/{:.0f}/{:.0f}\n", pl.name, pl.pos[0], pl.pos[1], pl.pos[2],
                           pl.lodDist[0], pl.lodDist[1], pl.lodDist[2]);
                for (const char band : { 'h', 'm', 'l' })
                {
                    if (const auto v = xi::dat::lodVariant(pl.name, band); v && zone.models.contains(*v) && *v != pl.name)
                    {
                        fmt::print("  lod {} '{}'\n", band, *v);
                        printModel(*v);
                    }
                }
                printModel(pl.name);
            }
            for (const auto& [chunk, modelName] : zone.modelByChunk)
            {
                if (!placed.contains(modelName))
                {
                    fmt::print("effect mesh chunk '{}' model '{}'\n", chunk, modelName);
                    printModel(modelName);
                }
            }
        }
        else if (cmd == "reach" && args.size() >= 3)
        {
            // Access check for the step-up height: flood-fill from every zone line (the zone's entrances) on a half-yalm grid,
            // moving with gfx::ZoneCollision::move in 8 directions, and count the reachable cells for each step-up value;
            // cells reached with the largest value but not with a smaller one are the access the smaller one loses.
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            xi::gfx::ZoneCollision col;
            col.addCollision(zone.collision);
            col.build();
            if (!col.hasCollision())
            {
                fmt::print("zone {}: no collision\n", zoneId);
                return 0;
            }
            using Key = std::tuple<int, int, int>;
            auto key  = [](const glm::vec3& p) { return Key{ static_cast<int>(std::floor(p.x * 2.0f)), static_cast<int>(std::floor(p.z * 2.0f)), static_cast<int>(std::floor(p.y * 2.0f)) }; };
            std::vector<std::pair<std::string, std::set<Key>>> results;
            std::map<Key, std::pair<glm::vec3, glm::vec3>>     parents; // last run: cell -> (from, to), for XI_REACH_LIST
            for (std::size_t a = 2; a < args.size(); ++a)
            {
                // "o0.75": the same step-up with the earlier wall slices (XI_MOVE_OLD) instead of the swept sphere.
                const bool  old  = args[a][0] == 'o';
                const float step = std::stof(args[a].substr(old ? 1 : 0));
                col.setStepUp(step);
                col.setSphere(!old);
                std::set<Key>         seen;
                std::deque<glm::vec3> todo;
                for (const auto& l : zone.zoneLines)
                {
                    const glm::vec3 c(l.pos[0], -l.pos[1], -l.pos[2]); // render space
                    if (const auto g = col.groundHeight(c.x, c.z, c.y + std::abs(l.size[1]) * 0.5f))
                    {
                        const glm::vec3 p(c.x, *g, c.z);
                        if (seen.insert(key(p)).second)
                        {
                            todo.push_back(p);
                        }
                    }
                }
                while (!todo.empty() && seen.size() < 3000000)
                {
                    const glm::vec3 p = todo.front();
                    todo.pop_front();
                    for (int d = 0; d < 8; ++d)
                    {
                        const float     ang = static_cast<float>(d) * 0.7853982f;
                        // Six frame-sized moves (run speed 5 yalms / s at 60 fps = 0.083): slopes are climbed as in the client.
                        glm::vec3 q = p;
                        for (int k = 0; k < 6; ++k)
                        {
                            q = col.move(q, glm::vec3(std::cos(ang), 0, std::sin(ang)) * (0.5f / 6.0f));
                        }
                        if (glm::distance(q, p) > 0.1f && seen.insert(key(q)).second)
                        {
                            if (a + 1 == args.size() && std::getenv("XI_REACH_LIST"))
                            {
                                parents[key(q)] = { p, q };
                            }
                            todo.push_back(q);
                        }
                    }
                }
                results.push_back({ fmt::format("{}{:.2f}", old ? "slices " : "", step), std::move(seen) });
            }
            // Compared on the ground plan (1-yalm x / z cells, height ignored): the same ground reached by another path can land in
            // another height bucket.
            auto plan = [](const std::set<Key>& cells) {
                std::set<std::pair<int, int>> out;
                for (const auto& k : cells)
                {
                    out.insert({ static_cast<int>(std::floor(std::get<0>(k) / 2.0)), static_cast<int>(std::floor(std::get<1>(k) / 2.0)) });
                }
                return out;
            };
            const auto most = plan(results.back().second);
            if (std::getenv("XI_REACH_LIST") && results.size() >= 2)
            {
                // Where the last run first gets to ground the first one never does: moves from a cell both reach.
                const auto& first = results.front().second;
                const auto  fplan = plan(first);
                int         shown = 0;
                if (const char* t = std::getenv("XI_REACH_TRACE")) // "x,z" (render plan cell): the last run's path back to the first's ground
                {
                    int tx = 0, tz = 0;
                    std::sscanf(t, "%d,%d", &tx, &tz);
                    for (const auto& [k, pq] : parents)
                    {
                        if (std::get<0>(k) / 2 != tx || std::get<1>(k) / 2 != tz)
                        {
                            continue;
                        }
                        Key cur = k;
                        for (int n = 0; n < 100000 && parents.contains(cur); ++n)
                        {
                            const auto& [p, q] = parents.at(cur);
                            // The first run's resolver on the same move (one of the 8 directions).
                            const float ang = std::round(std::atan2(q.z - p.z, q.x - p.x) / 0.7853982f) * 0.7853982f;
                            col.setSphere(true);
                            col.setStepUp(std::stof(args[2]));
                            glm::vec3 r = p;
                            for (int s = 0; s < 6; ++s)
                            {
                                r = col.move(r, glm::vec3(std::cos(ang), 0, std::sin(ang)) * (0.5f / 6.0f));
                            }
                            if (first.contains(key(p)) || n < 3 || std::getenv("XI_REACH_TRACE_ALL") || glm::distance(r, q) > 0.1f)
                            {
                                fmt::print("  trace {}: ({:.2f}, {:.2f}, {:.2f}) -> ({:.2f}, {:.2f}, {:.2f}), first run ({:.2f}, {:.2f}, {:.2f}){}\n", n, p.x, p.y, p.z, q.x, q.y, q.z, r.x, r.y, r.z,
                                           first.contains(key(p)) ? " (from shared ground)" : "");
                            }
                            if (first.contains(key(p)))
                            {
                                break;
                            }
                            cur = key(p);
                        }
                        break;
                    }
                }
                for (const auto& [k, pq] : parents)
                {
                    const auto& [p, q] = pq;
                    const std::pair<int, int> qc{ static_cast<int>(std::floor(std::get<0>(k) / 2.0)), static_cast<int>(std::floor(std::get<1>(k) / 2.0)) };
                    if (shown < 400 && first.contains(key(p)) && !fplan.contains(qc))
                    {
                        fmt::print("  only last: ({:.2f}, {:.2f}, {:.2f}) -> ({:.2f}, {:.2f}, {:.2f})\n", p.x, p.y, p.z, q.x, q.y, q.z);
                        ++shown;
                    }
                }
            }
            for (const auto& [step, cellsY] : results)
            {
                const auto  cells = plan(cellsY);
                std::size_t lost  = 0;
                std::map<std::pair<int, int>, int> where; // lost cells by 10-yalm block (game x, z)
                for (const auto& k : most)
                {
                    if (!cells.contains(k))
                    {
                        ++lost;
                        if (std::getenv("XI_REACH_LIST")) // the lost cells next to reached ones (render x, z)
                        {
                            for (const auto& [ox, oz] : { std::pair{ 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } })
                            {
                                if (cells.contains({ k.first + ox, k.second + oz }))
                                {
                                    float y = 0.0f;
                                    for (const auto& c : results.back().second)
                                    {
                                        if (std::get<0>(c) / 2 == k.first && std::get<1>(c) / 2 == k.second)
                                        {
                                            y = static_cast<float>(std::get<2>(c)) * 0.5f;
                                        }
                                    }
                                    fmt::print("  lost ({}, {}) y {:.1f} from ({}, {})\n", k.first, k.second, y, k.first + ox, k.second + oz);
                                    break;
                                }
                            }
                        }
                        ++where[{ static_cast<int>(std::floor(k.first / 10.0)) * 10, static_cast<int>(std::floor(-k.second / 10.0)) * 10 }];
                    }
                }
                fmt::print("zone {} step-up {}: {} square yalms reachable, {} of the last value's not reached\n", zoneId, step, cells.size(), lost);
                std::vector<std::pair<int, std::pair<int, int>>> top;
                for (const auto& [c, n] : where) top.push_back({ n, c });
                std::sort(top.rbegin(), top.rend());
                for (std::size_t i = 0; i < top.size() && i < 4 && lost > 0; ++i)
                {
                    fmt::print("    {} cells near game ({}, {})\n", top[i].first, top[i].second.first, top[i].second.second);
                }
            }
        }
        else if (cmd == "motions" && args.size() >= 2)
        {
            // Each Mo2 motion in the files, with the attachment bones (3..24, weapons and props, dat-format §10.2) it drives.
            for (std::size_t k = 1; k < args.size(); ++k)
            {
                const auto              file = static_cast<uint32_t>(std::stoul(args[k], nullptr, 0));
                std::vector<xi::dat::Chunk> chunks;
                const auto              data = idx.read(file);
                xi::dat::parseChunks(data, chunks);
                for (const auto& c : chunks)
                {
                    if (c.type != 43)
                    {
                        continue;
                    }
                    const auto                                m = xi::dat::Motion::parse(c.payload);
                    std::vector<xi::dat::Motion::BonePose>    pose(128);
                    m.sample(0.0f, pose);
                    std::vector<int> att;
                    int              n = 0;
                    for (std::size_t b = 0; b < pose.size(); ++b)
                    {
                        n += pose[b].driven;
                        if (pose[b].driven && b >= 3 && b <= 24) att.push_back(static_cast<int>(b));
                    }
                    std::string watch;
                    if (const char* w = std::getenv("XI_BONES"))
                    {
                        std::string list(w);
                        for (std::size_t at = 0; at < list.size();)
                        {
                            const std::size_t end = std::min(list.find(',', at), list.size());
                            const auto i = static_cast<std::size_t>(std::stoul(list.substr(at, end - at)));
                            at = end + 1;
                            watch += fmt::format(" {}:{}", i, i < pose.size() && pose[i].driven ? "Y" : "-");
                        }
                    }
                    fmt::print("{} {:4s} {:3d} frames, {:3d} bones, attachment {}{}\n", file, c.name, m.frames(), n, att, watch);
                }
            }
        }
        else if (cmd == "partbones" && args.size() >= 2)
        {
            // partbones npc MODEL | partbones pc RACE FACE H B HA L F MAIN SUB RANGED: the bones each mesh part uses.
            std::optional<xi::dat::CharacterAsset> asset;
            auto num = [&](std::size_t i) { return args.size() > i ? static_cast<uint16_t>(std::stoul(args[i], nullptr, 0)) : uint16_t{ 0 }; };
            if (args[1] == "npc") asset = xi::dat::loadNpcAsset(idx, num(2));
            else asset = xi::dat::loadPcAsset(idx, static_cast<uint8_t>(num(2)), static_cast<uint8_t>(num(3)), { num(4), num(5), num(6), num(7), num(8), num(9), num(10), num(11) });
            if (!asset) { fmt::print("no model\n"); return 1; }
            for (const auto& part : asset->parts)
            {
                std::set<int> bones;
                for (uint32_t v = part.first; v < part.first + part.count && v < asset->vertices.size(); ++v)
                {
                    bones.insert(asset->vertices[v].bone0);
                    if (asset->vertices[v].weight1 > 0.0f) bones.insert(asset->vertices[v].bone1);
                }
                fmt::print("part {:16s} wep {:2d} {:5d} verts, bones {}\n", part.texture, part.wep, part.count, bones);
            }
            fmt::print("motion files {} battle {}\n", asset->motionFiles, asset->battleMotionFiles);
            for (std::size_t b = 0; b < asset->skeleton.bones.size() && b < 40; ++b)
            {
                const auto& bn = asset->skeleton.bones[b];
                const auto& w  = asset->skeleton.bindWorld[b];
                fmt::print("bone {:2d} parent {:2d} t ({:6.3f} {:6.3f} {:6.3f}) world ({:6.3f} {:6.3f} {:6.3f})\n", b, bn.parent, bn.translation[0], bn.translation[1], bn.translation[2], w[12], w[13], w[14]);
            }
        }
        else if (cmd == "attachpos" && args.size() >= 4)
        {
            // attachpos RACE STEM FILE...: world positions of every bone, posed by STEM's frame 0.
            const auto asset = xi::dat::loadPcAsset(idx, static_cast<uint8_t>(std::stoul(args[1])), 1, { 0, 8, 8, 8, 8, 0, 0, 0 });
            std::vector<uint32_t> files;
            for (std::size_t k = 3; k < args.size(); ++k) files.push_back(static_cast<uint32_t>(std::stoul(args[k])));
            const auto colon = args[2].find(':');
            const auto set   = xi::dat::loadMotionSet(idx, files, args[2].substr(0, colon));
            const float frame = colon == std::string::npos ? 0.0f : std::stof(args[2].substr(colon + 1));
            std::vector<xi::dat::RowMatrix> out;
            xi::dat::poseSkeleton(asset->skeleton, { &set }, frame, out);
            for (std::size_t b = 0; b < out.size(); ++b)
            {
                fmt::print("bone {:2d} parent {:2d} ({:6.3f} {:6.3f} {:6.3f})\n", b, asset->skeleton.bones[b].parent, out[b][12], out[b][13], out[b][14]);
            }
        }
        else if (cmd == "route" && args.size() >= 6)
        {
            // Walking route for test scripts: A* over a half-yalm grid from game (x, z) to (x, z), each step frame-sized
            // gfx::ZoneCollision::move calls (as reach), then shortened to the waypoints a straight run still reaches.
            // Prints "x z" pairs (game space) for a script's goto line. Doors are left open (the client opens them).
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            xi::gfx::ZoneCollision col;
            col.addCollision(zone.collision);
            col.build();
            const glm::vec2 from(std::stof(args[2]), std::stof(args[3]));
            const glm::vec2 to(std::stof(args[4]), std::stof(args[5]));
            const float     fromY = args.size() >= 7 ? std::stof(args[6]) : -1000.0f; // game y (down): start on the floor below it
            const auto      g0    = col.groundHeight(from.x, -from.y, -fromY + 2.0f);
            if (!col.hasCollision() || !g0)
            {
                fmt::print(stderr, "zone {}: no collision / no floor at the start\n", zoneId);
                return 1;
            }
            const glm::vec3 start(from.x, *g0, -from.y);
            using Key = std::tuple<int, int, int>;
            auto key  = [](const glm::vec3& p) { return Key{ static_cast<int>(std::floor(p.x * 2.0f)), static_cast<int>(std::floor(p.z * 2.0f)), static_cast<int>(std::floor(p.y)) }; };
            auto walk = [&](glm::vec3 p, glm::vec2 dir, float len) {
                const int n = std::max(1, static_cast<int>(std::ceil(len / 0.083f)));
                for (int k = 0; k < n; ++k)
                {
                    p = col.move(p, glm::vec3(dir.x, 0, dir.y) * (len / static_cast<float>(n)));
                }
                return p;
            };
            const glm::vec2 goal(to.x, -to.y); // render x / z
            struct Node
            {
                glm::vec3 p;
                float     g = 0.0f;
                Key       parent;
            };
            std::map<Key, Node> nodes;
            using Open = std::pair<float, Key>;
            std::priority_queue<Open, std::vector<Open>, std::greater<>> open;
            nodes[key(start)] = { start, 0.0f, key(start) };
            open.push({ glm::distance(glm::vec2(start.x, start.z), goal), key(start) });
            std::optional<Key> found;
            while (!open.empty() && nodes.size() < 4000000)
            {
                const auto [f, k] = open.top();
                open.pop();
                const Node cur = nodes[k];
                if (glm::distance(glm::vec2(cur.p.x, cur.p.z), goal) < 0.75f)
                {
                    found = k;
                    break;
                }
                if (f > cur.g + glm::distance(glm::vec2(cur.p.x, cur.p.z), goal) + 0.01f)
                {
                    continue; // stale
                }
                for (int d = 0; d < 8; ++d)
                {
                    const float     ang = static_cast<float>(d) * 0.7853982f;
                    const glm::vec2 dir(std::cos(ang), std::sin(ang));
                    const glm::vec3 q     = walk(cur.p, dir, d % 2 ? 0.7071f : 0.5f);
                    const float     moved = glm::distance(glm::vec2(q.x, q.z), glm::vec2(cur.p.x, cur.p.z));
                    if (moved < 0.2f)
                    {
                        continue;
                    }
                    const Key   qk = key(q);
                    const float g  = cur.g + moved;
                    const auto  it = nodes.find(qk);
                    if (it == nodes.end() || g < it->second.g)
                    {
                        nodes[qk] = { q, g, k };
                        open.push({ g + glm::distance(glm::vec2(q.x, q.z), goal), qk });
                    }
                }
            }
            if (!found)
            {
                fmt::print(stderr, "zone {}: no route ({} cells searched)\n", zoneId, nodes.size());
                return 1;
            }
            std::vector<glm::vec3> path;
            for (Key k = *found;; k = nodes[k].parent)
            {
                path.push_back(nodes[k].p);
                if (nodes[k].parent == k)
                {
                    break;
                }
            }
            std::reverse(path.begin(), path.end());
            path.push_back(glm::vec3(goal.x, path.back().y, goal.y));
            // Shorten: from each waypoint, the farthest later one a straight run reaches (within 0.4).
            std::vector<glm::vec3> way{ path.front() };
            std::size_t            i = 0;
            while (i + 1 < path.size())
            {
                std::size_t best = i + 1;
                for (std::size_t j = path.size() - 1; j > i + 1; --j)
                {
                    const glm::vec2 a(path[i].x, path[i].z), b(path[j].x, path[j].z);
                    const glm::vec3 q = walk(path[i], glm::normalize(b - a), glm::distance(a, b));
                    if (glm::distance(glm::vec2(q.x, q.z), b) < 0.4f && std::abs(q.y - path[j].y) < 2.0f)
                    {
                        best = j;
                        break;
                    }
                }
                way.push_back(path[best]);
                i = best;
            }
            fmt::print(stderr, "zone {}: {} cells searched, path {:.0f} yalms, {} waypoints\n", zoneId, nodes.size(), nodes[*found].g, way.size() - 1);
            for (std::size_t w = 1; w < way.size(); ++w)
            {
                fmt::print("{:.1f} {:.1f} ", way[w].x, -way[w].z);
            }
            fmt::print("\n");
        }
        else if (cmd == "camat" && args.size() >= 4)
        {
            // camat ZONE X Z [PITCH]: the client's camera distance (8 wanted) at render (X, Z), feet on the highest floor
            // there, for the client's --yaw every 15 degrees (pitch default -20, the client's).
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            xi::gfx::ZoneCollision col;
            col.addCollision(zone.collision);
            col.build();
            const float x = std::stof(args[2]), z = std::stof(args[3]);
            const float pitch = (args.size() >= 5 ? std::stof(args[4]) : -20.0f) * 3.14159265f / 180.0f;
            const auto  g     = col.groundHeight(x, z, 1e9f);
            if (!g)
            {
                fmt::print("no ground\n");
                return 1;
            }
            const glm::vec3 target(x, *g + 1.5f, z);
            for (int deg = 0; deg < 360; deg += 15)
            {
                const float     yaw = static_cast<float>(deg) * 3.14159265f / 180.0f;
                const glm::vec3 fwd(std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)); // gfx::FlyCamera
                fmt::print("yaw {:3}: {:.2f}\n", deg, col.cameraDistance(target, fwd, 8.0f, 0.5f));
            }
        }
        else if (cmd == "camsim" && args.size() >= 4)
        {
            // camsim ZONE GX GZ [TOX TOZ [SECONDS]]: client::GameCamera with the zone's collision at 60 ticks a second:
            // placed behind the character standing at game (GX, GZ) facing (TOX, TOZ), which it then runs to at 5 a
            // second (the client's run) and stands for SECONDS (default 6). Prints the eye every 0.1 s and the frames
            // where it jumps more than 0.05 while the character stands still (XI_CAMSIM_ALL=1: every frame).
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            xi::gfx::ZoneCollision col;
            col.addCollision(zone.collision);
            col.build();
            const glm::vec2 from(std::stof(args[2]), -std::stof(args[3])); // render x / z
            const glm::vec2 to   = args.size() >= 6 ? glm::vec2(std::stof(args[4]), -std::stof(args[5])) : from;
            const float     hold = args.size() >= 7 ? std::stof(args[6]) : 6.0f;
            const auto      g    = col.groundHeight(from.x, from.y, 1e9f);
            if (!g)
            {
                fmt::print("no ground\n");
                return 1;
            }
            xi::client::GameCamera::Subject s;
            s.feet          = glm::vec3(from.x, *g, from.y);
            const glm::vec2 dir0 = glm::length(to - from) > 0.01f ? glm::normalize(to - from) : glm::vec2(0.0f, -1.0f);
            s.facing        = glm::vec3(dir0.x, 0.0f, dir0.y);
            s.height        = 1.8f;
            xi::client::GameCamera cam;
            cam.reset(s);
            const auto ray   = [&](const glm::vec3& a, const glm::vec3& b) { return col.cameraRay(a, b); };
            const auto clear = [&](const glm::vec3& e, const glm::vec3& f) { return col.cameraClearance(e, f); };
            const bool all   = std::getenv("XI_CAMSIM_ALL") != nullptr;
            int        jumps = 0;
            glm::vec3  last  = cam.eye();
            for (int f = 0; f < 60 * 60; ++f)
            {
                const glm::vec2 here(s.feet.x, s.feet.z);
                const bool      walking = glm::distance(here, to) > 0.05f && f < static_cast<int>(std::ceil(glm::distance(from, to) * 12.0f)); // 5 a second, as long as it takes in open ground
                if (walking)
                {
                    const glm::vec2 step = glm::normalize(to - here) * std::min(5.0f / 60.0f, glm::distance(here, to));
                    s.feet               = col.move(s.feet, glm::vec3(step.x, 0.0f, step.y));
                }
                cam.update(s, {}, std::nullopt, false, 1.0f, ray, clear);
                const float jump = glm::distance(cam.eye(), last);
                last             = cam.eye();
                if (!walking && jump > 0.05f)
                {
                    ++jumps;
                }
                if (all || f % 6 == 0 || (!walking && jump > 0.05f))
                {
                    fmt::print("{:5.2f}s feet ({:.2f}, {:.2f}, {:.2f}) eye ({:.2f}, {:.2f}, {:.2f}) dist {:.2f} rise {:.2f}{}\n", static_cast<float>(f) / 60.0f, s.feet.x, -s.feet.y,
                               -s.feet.z, cam.eye().x, -cam.eye().y, -cam.eye().z, cam.distance(), cam.eye().y - cam.at().y,
                               !walking && jump > 0.05f ? fmt::format(" JUMP {:.2f}", jump) : "");
                }
                static int stoodFrames = 0;
                stoodFrames            = walking ? 0 : stoodFrames + 1;
                if (stoodFrames > static_cast<int>(hold * 60.0f))
                {
                    break;
                }
            }
            fmt::print("jumps while standing: {}\n", jumps);
        }
        else if (cmd == "camcheck" && args.size() >= 2)
        {
            // Third-person camera check: from ground reachable from the zone lines, the default camera (8 back, 20 degrees
            // down, 1.5 above the feet) at 8 headings; counts eyes cut off from the character (behind a surface seen from
            // both the target and 1 above the feet; XI_CAM_UNDER=1 lists some) and views whose lower frustum meets
            // geometry nearer than 0.5 (a magnified texture across the bottom of the screen), with and without
            // ZoneCollision::cameraClearance.
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const int  want   = args.size() > 2 ? std::stoi(args[2]) : 3000;
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            xi::gfx::ZoneCollision col;
            col.addCollision(zone.collision);
            col.build();
            if (!col.hasCollision())
            {
                fmt::print("zone {}: no collision\n", zoneId);
                return 0;
            }
            std::vector<glm::vec3> ground;
            {
                using Key = std::pair<int, int>;
                std::set<Key>         seen;
                std::deque<glm::vec3> todo;
                for (const auto& l : zone.zoneLines)
                {
                    const glm::vec3 c(l.pos[0], -l.pos[1], -l.pos[2]);
                    if (const auto g = col.groundHeight(c.x, c.z, c.y + std::abs(l.size[1]) * 0.5f))
                    {
                        todo.push_back({ c.x, *g, c.z });
                    }
                }
                while (!todo.empty() && ground.size() < 200000)
                {
                    const glm::vec3 p = todo.front();
                    todo.pop_front();
                    if (!seen.insert({ static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.z)) }).second)
                    {
                        continue;
                    }
                    ground.push_back(p);
                    for (int d = 0; d < 4; ++d)
                    {
                        const glm::vec3 step(d == 0 ? 1.0f : d == 1 ? -1.0f : 0.0f, 0.0f, d == 2 ? 1.0f : d == 3 ? -1.0f : 0.0f);
                        glm::vec3       q = p;
                        for (int k = 0; k < 12; ++k)
                        {
                            q = col.move(q, step * (1.0f / 12.0f));
                        }
                        if (glm::distance(q, p) > 0.5f)
                        {
                            todo.push_back(q);
                        }
                    }
                }
            }
            if (ground.empty())
            {
                fmt::print("zone {}: no reachable ground\n", zoneId);
                return 0;
            }
            std::mt19937 rng(1234);
            const float  pitch = glm::radians(-20.0f), halfV = glm::radians(30.0f), halfH = std::atan(std::tan(halfV) * 16.0f / 9.0f);
            int          samples = 0, under[3] = { 0, 0, 0 }, column[3] = { 0, 0, 0 }, close[3] = { 0, 0, 0 };
            bool         nearAt[3] = { false, false, false };
            const bool   verbose   = std::getenv("XI_CAM_VERBOSE") != nullptr;
            int          shown     = 0;
            const bool   verboseUnder = std::getenv("XI_CAM_UNDER") != nullptr;
            int          shownUnder   = 0;
            for (int n = 0; n < want; ++n)
            {
                const glm::vec3 feet = ground[rng() % ground.size()];
                for (int h = 0; h < 8; ++h)
                {
                    const float     yaw = static_cast<float>(h) * 0.7853982f;
                    const glm::vec3 fwd(std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw));
                    const glm::vec3 target = feet + glm::vec3(0.0f, 1.5f, 0.0f);
                    const glm::vec3 base   = target - fwd * col.cameraDistance(target, fwd, 8.0f);
                    const glm::vec3 low    = target - fwd * col.cameraDistance(target, fwd, 8.0f, 0.5f);
                    ++samples;
                    for (int c = 0; c < 3; ++c)
                    {
                        const glm::vec3 eye = c == 0 ? base : c == 1 ? col.cameraClearance(base, fwd) : col.cameraClearance(low, fwd);
                        // The old column test (a floor less than 3 above the eye, or none below it) also counts eyes
                        // under bridges, overhangs and in caves.
                        if (const auto g = col.groundHeight(eye.x, eye.z, eye.y + 50.0f); g && *g > eye.y)
                        {
                            const auto below = col.groundHeight(eye.x, eye.z, eye.y);
                            if (!below || *g - eye.y < 3.0f)
                            {
                                ++column[c];
                            }
                        }
                        // Cut off: the eye is behind a surface, seen from both the view target and 1 above the feet
                        // (two-sided triangles, barriers skipped as the camera ray does). An eye in open air under a
                        // bridge or in a cave still sees the character.
                        auto blocked = [&](const glm::vec3& from) {
                            const glm::vec3 d   = eye - from;
                            const float     len = glm::length(d);
                            return len > 0.06f && col.rayHit(from, d / len, len, true).has_value();
                        };
                        if (blocked(target) && blocked(feet + glm::vec3(0.0f, 1.0f, 0.0f)))
                        {
                            ++under[c];
                            if (verboseUnder && shownUnder < 8)
                            {
                                ++shownUnder;
                                fmt::print("  cut-off eye ({}) at game ({:.1f}, {:.1f}, {:.1f}) feet ({:.1f}, {:.1f}, {:.1f})\n", c, eye.x, -eye.y,
                                           -eye.z, feet.x, -feet.y, -feet.z);
                            }
                            continue;
                        }
                        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
                        const glm::vec3 up    = glm::cross(right, fwd);
                        bool            near  = false;
                        for (int iy = 0; iy <= 2 && !near; ++iy)
                        {
                            for (int ix = -2; ix <= 2 && !near; ++ix)
                            {
                                const float     ay = -halfV * static_cast<float>(iy) / 2.0f, ax = halfH * static_cast<float>(ix) / 2.0f;
                                const glm::vec3 d  = glm::normalize(fwd + up * std::tan(ay) + right * std::tan(ax));
                                if (const auto t = col.rayHit(eye, d, 0.5f))
                                {
                                    near = true;
                                }
                            }
                        }
                        close[c] += near ? 1 : 0;
                        nearAt[c] = near;
                    }
                    // XI_CAM_VERBOSE=1: the first views that were close and are now clear (game coordinates, heading).
                    if (verbose && nearAt[0] && !nearAt[2] && shown < 8)
                    {
                        ++shown;
                        fmt::print("  fixed view at game ({:.1f}, {:.1f}, {:.1f}) yaw {} deg\n", feet.x, -feet.y, -feet.z, h * 45);
                    }
                }
            }
            fmt::print("zone {}: {} views (head ray / + clearance / + ray from 1 above the feet); eye cut off {} / {} / {} (column "
                       "test {} / {} / {}); lower view nearer than 0.5: {:.2f}% / {:.2f}% / {:.2f}%\n",
                       zoneId, samples, under[0], under[1], under[2], column[0], column[1], column[2], 100.0 * close[0] / samples,
                       100.0 * close[1] / samples, 100.0 * close[2] / samples);
            // The game camera (client::GameCamera), want / 4 runs: placed behind the character, which runs 4 one way at 5
            // a second and stands 3 s. Counted at the end: eyes cut off from the look-at, eyes nearer than 1; while
            // standing (after 1 s): frames the eye jumps more than 0.05 (a bounce).
            int        runs = 0, cut = 0, nearEye = 0, bounce = 0, bouncing = 0;
            const auto ray  = [&](const glm::vec3& a, const glm::vec3& b) { return col.cameraRay(a, b); };
            const auto clr  = [&](const glm::vec3& e, const glm::vec3& f) { return col.cameraClearance(e, f); };
            for (int n = 0; n < want / 4; ++n)
            {
                xi::client::GameCamera::Subject s;
                s.feet          = ground[rng() % ground.size()];
                const glm::vec3 start = s.feet;
                const float yaw = static_cast<float>(rng() % 360) * 0.0174533f;
                s.facing        = glm::vec3(std::sin(yaw), 0.0f, -std::cos(yaw));
                xi::client::GameCamera cam;
                cam.reset(s);
                glm::vec3 last  = cam.eye();
                int       mine  = 0;
                for (int f = 0; f < 60 * 4; ++f)
                {
                    if (f < 48)
                    {
                        s.feet = col.move(s.feet, s.facing * (5.0f / 60.0f));
                    }
                    cam.update(s, {}, std::nullopt, false, 1.0f, ray, clr);
                    if (f > 48 + 60 && glm::distance(cam.eye(), last) > 0.05f)
                    {
                        ++mine;
                    }
                    last = cam.eye();
                }
                if (verbose && mine > 0 && bouncing < 6) // XI_CAM_VERBOSE=1: where (camsim reruns one)
                {
                    const glm::vec3 from = start + s.facing * 4.0f; // where the run was headed
                    fmt::print("  bounce x{}: camsim {} {:.2f} {:.2f} {:.2f} {:.2f}\n", mine, zoneId, start.x, -start.z, from.x, -from.z);
                }
                ++runs;
                bounce += mine;
                bouncing += mine > 0 ? 1 : 0;
                if (const auto h = col.cameraRay(cam.at(), cam.eye()); h && *h < cam.distance() - 0.01f)
                {
                    ++cut;
                }
                nearEye += cam.distance() < 1.0f ? 1 : 0;
            }
            fmt::print("zone {}: game camera, {} runs: eye cut off {}, nearer than 1: {}, bouncing runs {} ({} frames)\n", zoneId, runs, cut,
                       nearEye, bouncing, bounce);
        }
        else if (cmd == "furn")
        {
            // furn [MAXH]: furniture rows (dat-format §7.8b "Furniture") no taller than MAXH (0.01 units; default all):
            // flags, footprint w x d (0.6 cells), height and the item's name.
            const int                maxH = args.size() >= 2 ? std::stoi(args[1]) : 1 << 30;
            const xi::dat::ItemTable items(idx);
            for (const auto& r : xi::dat::loadFurnitureTable(idx))
            {
                if (r.height > maxH)
                {
                    continue;
                }
                const auto info = items.find(r.itemId);
                fmt::print("{:5} flags {:2} {}x{} h {:4}  {}\n", r.itemId, r.flags, r.w, r.d, r.height, info ? info->name : "?");
            }
        }
        else if (cmd == "colnear" && args.size() >= 4)
        {
            // colnear ZONE X Z [R]: collision triangles with a vertex within R (default 2) of render (X, Z): render-space
            // corners (y up) and the render normal, steep ones (|ny| < 0.5) marked as walls.
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            const glm::vec2 at(std::stof(args[2]), std::stof(args[3]));
            const float     r = args.size() >= 5 ? std::stof(args[4]) : 2.0f;
            for (const auto& t : zone.collision)
            {
                bool near = false;
                for (const auto& v : t.v)
                {
                    near |= glm::distance(glm::vec2(v[0], -v[2]), at) <= r;
                }
                if (!near)
                {
                    continue;
                }
                const float ny = -t.normal[1];
                fmt::print("{}{} m{} n ({:.2f}, {:.2f}, {:.2f}):", std::abs(ny) < 0.5f ? "wall " : "floor", t.barrier ? " barrier" : "", t.material, t.normal[0], ny,
                           -t.normal[2]);
                for (const auto& v : t.v)
                {
                    fmt::print(" ({:.2f}, {:.2f}, {:.2f})", v[0], -v[1], -v[2]);
                }
                fmt::print("\n");
            }
        }
        else if (cmd == "npcground" && args.size() >= 2)
        {
            // npcground ZONE < lines "ID X Y Z KEEP [NAME...]" (game space, Y down; KEEP 1 = the 0x00D / 0x00E GroundFlag):
            // where the game's actor ground probe (client::actorGround) stands each one. Prints one line per actor (the
            // server y, the height drawn, and "keep" / "ground" / "miss") and a summary of how far they moved;
            // tools/npc_ground_check.py feeds it LSB's npcs.yaml.
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            xi::gfx::ZoneCollision col;
            col.addCollision(zone.collision);
            col.build();
            int         total = 0, kept = 0, grounded = 0, missed = 0, down = 0, up = 0;
            std::string line;
            while (std::getline(std::cin, line))
            {
                std::istringstream in(line);
                std::string        id;
                float              x = 0, y = 0, z = 0;
                int                keep = 0;
                if (!(in >> id >> x >> y >> z >> keep))
                {
                    continue;
                }
                std::string name;
                std::getline(in, name);
                ++total;
                const auto  g     = xi::client::actorGround(-y, [&](float maxY) { return col.groundHeight(x, -z, maxY); },
                                                            xi::client::actorGroundAbove(zoneId));
                float       drawn = y;
                const char* what  = "miss";
                if (keep != 0)
                {
                    what = "keep", ++kept;
                }
                else if (g)
                {
                    what = "ground", ++grounded, drawn = -*g;
                    down += drawn - y > 0.05f ? 1 : 0; // game y down: a larger y is lower
                    up += y - drawn > 0.05f ? 1 : 0;
                }
                else
                {
                    ++missed;
                }
                fmt::print("{} {} y {:.3f} drawn {:.3f} {}\n", id, what, y, drawn, name);
            }
            fmt::print("zone {}: {} actors, {} keep their height (GroundFlag), {} on the floor ({} lowered, {} raised more than "
                       "0.05), {} with no floor in reach\n",
                       zoneId, total, kept, grounded, down, up, missed);
        }
        else if (cmd == "unplaced" && args.size() >= 2)
        {
            // unplaced ZONE...: the zone's Mmbs that the Mzb does not place (dat-format §5.7), and which of them a
            // generator in the zone file draws as its particle (resource types 0x0b / 0x0c / 0x1f with no D3m of that
            // name: effect-format §Z.2). The rest are drawn by nothing we know of.
            const auto keys = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            std::map<std::string, int> unusedNames;
            int                        zones = 0, unplacedAll = 0, usedAll = 0, lodAll = 0, placementsAll = 0, lodPlacements = 0, nearDiffers = 0;
            std::map<char, int>        differBy;
            int                        lodDistShown = 0;
            for (std::size_t a = 1; a < args.size(); ++a)
            {
                const auto zoneId = static_cast<uint16_t>(std::stoul(args[a], nullptr, 0));
                const auto model  = xi::dat::zoneModelId(zoneId);
                xi::dat::ZoneData zone;
                try
                {
                    zone = xi::dat::loadZone(idx, model, keys);
                }
                catch (const std::exception&)
                {
                    continue;
                }
                std::set<std::string> placed;
                for (const auto& pl : zone.placements)
                {
                    placed.insert(pl.name);
                }
                std::set<uint32_t> d3m, drawn; // D3m names; mesh-particle resource names
                try
                {
                    const xi::dat::EffectFile fx(idx, xi::dat::zoneModelFileId(model));
                    for (const auto& r : fx.resources())
                    {
                        if (r.type == xi::dat::FxType::D3m)
                        {
                            d3m.insert(r.name);
                        }
                        if (r.type != xi::dat::FxType::Generator || r.payload.size() < 0x80)
                        {
                            continue;
                        }
                        const auto l1 = xi::load<uint32_t>(r.payload, 0x74);
                        const auto h  = l1 >= 0x10 ? l1 - 0x10 : 0;
                        if (h + 0x24 > r.payload.size())
                        {
                            continue;
                        }
                        const auto type = static_cast<uint8_t>(xi::load<int8_t>(r.payload, h + 0x21));
                        if (type == 0x0b || type == 0x0c || type == 0x1f)
                        {
                            drawn.insert(xi::load<uint32_t>(r.payload, h + 0x0C));
                        }
                    }
                }
                catch (const std::exception&)
                {
                }
                // LOD variants (CODE 0x1017aef0): a placed name ending in h / m / l also loads <stem>h / m / l.
                std::set<std::string> lodStems;
                for (const auto& n : placed)
                {
                    if (const auto s = xi::dat::lodStem(n))
                    {
                        lodStems.insert(*s);
                    }
                }
                // Placements whose near mesh (h, else m, else the name itself; CODE 0x1017aef0) is not the one named.
                for (const auto& pl : zone.placements)
                {
                    const auto h = xi::dat::lodVariant(pl.name, 'h'), m = xi::dat::lodVariant(pl.name, 'm');
                    const auto pick = h && zone.models.contains(*h) ? *h : m && zone.models.contains(*m) ? *m : pl.name;
                    ++placementsAll;
                    lodPlacements += h ? 1 : 0;
                    nearDiffers += pick != pl.name ? 1 : 0;
                    if (pick != pl.name && std::getenv("XI_LOD_DIST") && lodDistShown++ < 12)
                    {
                        fmt::print("  {} at ({:.0f}, {:.0f}): d {:.1f} {:.1f} {:.1f}\n", pl.name, pl.pos[0], pl.pos[2], pl.lodDist[0], pl.lodDist[1], pl.lodDist[2]);
                    }
                    if (pick != pl.name)
                    {
                        ++differBy[(*xi::dat::lodStem(pl.name)).find('?') != std::string::npos ? pl.name[(*xi::dat::lodStem(pl.name)).find('?')] : '-'];
                    }
                }
                int unplaced = 0, used = 0, lod = 0;
                std::vector<std::string> unused, usedNames;
                for (const auto& [chunk, modelName] : zone.modelByChunk)
                {
                    if (placed.contains(modelName))
                    {
                        continue;
                    }
                    ++unplaced;
                    const auto name = xi::dat::fxName(chunk);
                    if (const auto s = xi::dat::lodStem(modelName); s && lodStems.contains(*s))
                    {
                        ++lod;
                    }
                    else if (chunk.size() <= 4 && drawn.contains(name) && !d3m.contains(name))
                    {
                        ++used;
                        usedNames.push_back(modelName.substr(0, modelName.find_last_not_of(' ') + 1));
                    }
                    else
                    {
                        unused.push_back(modelName);
                        ++unusedNames[chunk];
                    }
                }
                std::ranges::sort(unused);
                ++zones, unplacedAll += unplaced, usedAll += used, lodAll += lod;
                if (unplaced)
                {
                    std::ranges::sort(usedNames);
                    fmt::print("zone {}: {} unplaced Mmbs, {} LOD variants of placed ones, {} drawn by zone generators ({}); not drawn: {}\n",
                               zoneId, unplaced, lod, used, fmt::join(usedNames, " | "), fmt::join(unused, " | "));
                }
            }
            fmt::print("{} zones: {} unplaced Mmbs, {} LOD variants, {} drawn by generators\n", zones, unplacedAll, lodAll, usedAll);
            fmt::print("{} placements, {} with an h / m / l name, {} whose near mesh is another variant\n", placementsAll, lodPlacements, nearDiffers);
            for (const auto& [c, n] : differBy)
            {
                fmt::print("  named ..{}: {}\n", c, n);
            }
            std::vector<std::pair<int, std::string>> top;
            for (const auto& [n, c] : unusedNames)
            {
                top.emplace_back(c, n);
            }
            std::ranges::sort(top, std::greater{});
            for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 40); ++i)
            {
                fmt::print("  {:<6} in {} zones\n", top[i].second, top[i].first);
            }
        }
        else if (cmd == "materials" && args.size() >= 2)
        {
            // materials ZONE [M [N]]: floor triangles per collision material (dat-format §7), and with M the game-space
            // centres of the first N (20) floors of that material (finding deep water, 9, for the wake test).
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            std::map<int, int> count;
            int                shown = 0;
            for (const auto& t : zone.collision)
            {
                if (t.normal[1] > -0.5f)
                {
                    continue; // walls and ceilings
                }
                ++count[t.material];
                if (args.size() >= 3 && t.material == std::stoi(args[2]) && shown++ < (args.size() >= 4 ? std::stoi(args[3]) : 20))
                {
                    fmt::print("  ({:.1f}, {:.1f}, {:.1f})\n", (t.v[0][0] + t.v[1][0] + t.v[2][0]) / 3, (t.v[0][1] + t.v[1][1] + t.v[2][1]) / 3,
                               (t.v[0][2] + t.v[1][2] + t.v[2][2]) / 3);
                }
            }
            for (const auto& [m, n] : count)
            {
                fmt::print("material {}: {} floors\n", m, n);
            }
        }
        else if (cmd == "waterreach" && args.size() >= 2)
        {
            // waterreach ZONE [STEP...]: flood-fill from the zone lines as `reach` does (default step-up kStepUp) and list
            // where the walk first gets from dry ground onto a water floor (collision material 8 shallow / 9 deep): the
            // places a player can walk into the sea. Game coordinates.
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            xi::gfx::ZoneCollision col;
            col.addCollision(zone.collision);
            for (const auto& b : zone.doorBoxes)
            {
                col.addDoorBox(b);
            }
            col.build();
            std::vector<float> steps;
            for (std::size_t a = 2; a < args.size(); ++a)
            {
                steps.push_back(std::stof(args[a]));
            }
            if (steps.empty())
            {
                steps.push_back(xi::gfx::ZoneCollision::kStepUp);
            }
            using Key = std::tuple<int, int, int>;
            auto key  = [](const glm::vec3& p) { return Key{ static_cast<int>(std::floor(p.x * 2.0f)), static_cast<int>(std::floor(p.z * 2.0f)), static_cast<int>(std::floor(p.y * 2.0f)) }; };
            for (const float step : steps)
            {
                col.setStepUp(step);
                std::set<Key>         seen;
                std::deque<glm::vec3> todo;
                for (const auto& l : zone.zoneLines)
                {
                    const glm::vec3 c(l.pos[0], -l.pos[1], -l.pos[2]);
                    if (const auto g = col.groundHeight(c.x, c.z, c.y + std::abs(l.size[1]) * 0.5f))
                    {
                        const glm::vec3 p(c.x, *g, c.z);
                        if (seen.insert(key(p)).second)
                        {
                            todo.push_back(p);
                        }
                    }
                }
                int                                       wet = 0, deep = 0;
                std::map<std::pair<int, int>, glm::vec3>  entries; // 4-yalm cells -> one dry -> wet step (from)
                std::map<std::pair<int, int>, glm::vec3>  entryTo;
                auto wetAt = [&](const glm::vec3& p) {
                    // Standing on the water floor itself (not on a seam's steep face above it).
                    const auto g = col.groundHeight(p.x, p.z, p.y + 0.05f);
                    const auto m = g && std::abs(*g - p.y) < 0.05f ? col.materialAt(p.x, p.z, p.y + 0.05f) : 0;
                    return m == 8 || m == 9 ? static_cast<int>(m) : 0;
                };
                while (!todo.empty() && seen.size() < 3000000)
                {
                    const glm::vec3 p = todo.front();
                    todo.pop_front();
                    const int pw = wetAt(p);
                    wet += pw != 0;
                    deep += pw == 9;
                    for (int d = 0; d < 8; ++d)
                    {
                        const float ang = static_cast<float>(d) * 0.7853982f;
                        glm::vec3   q   = p;
                        for (int k = 0; k < 6; ++k)
                        {
                            q = col.move(q, glm::vec3(std::cos(ang), 0, std::sin(ang)) * (0.5f / 6.0f));
                        }
                        if (glm::distance(q, p) > 0.1f && seen.insert(key(q)).second)
                        {
                            if (pw == 0 && wetAt(q) == 9)
                            {
                                const std::pair<int, int> cell{ static_cast<int>(std::floor(p.x / 4)), static_cast<int>(std::floor(-p.z / 4)) };
                                entries.try_emplace(cell, p);
                                entryTo.try_emplace(cell, q);
                            }
                            todo.push_back(q);
                        }
                    }
                }
                fmt::print("zone {} step-up {:.2f}: {} cells reached, {} on water ({} deep), {} places dry -> deep\n", zoneId, step, seen.size(), wet, deep,
                           entries.size());
                int shown = 0;
                for (const auto& [cell, p] : entries)
                {
                    if (shown++ >= 30)
                    {
                        break;
                    }
                    const auto& q = entryTo[cell];
                    fmt::print("  ({:.2f}, {:.2f}, {:.2f}) -> ({:.2f}, {:.2f}, {:.2f})\n", p.x, -p.y, -p.z, q.x, -q.y, -q.z);
                }
            }
        }
        else if (cmd == "movetest" && args.size() >= 5)
        {
            // movetest ZONE X Z TOX TOZ [MAXY] (render space, y from the highest floor at X, Z below MAXY): walks frame-sized steps toward
            // (TOX, TOZ) with the swept sphere and the old wall slices and prints where each one gets to.
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            xi::gfx::ZoneCollision col;
            col.addCollision(zone.collision);
            for (const auto& b : zone.doorBoxes)
            {
                col.addDoorBox(b); // closed, as the client starts them
            }
            col.build();
            const glm::vec2 from(std::stof(args[2]), std::stof(args[3])), to(std::stof(args[4]), std::stof(args[5]));
            const float     maxY = args.size() >= 7 ? std::stof(args[6]) : 1e9f;
            // XI_MOVETEST_STEPUP=H: the step-up (0.5 = the game's pure sphere; default kStepUp).
            if (const char* su = std::getenv("XI_MOVETEST_STEPUP"))
            {
                col.setStepUp(std::stof(su));
            }
            for (const bool sphere : { true, false })
            {
                col.setSphere(sphere);
                const auto g = col.groundHeight(from.x, from.y, maxY);
                if (!g)
                {
                    fmt::print("no ground\n");
                    return 1;
                }
                glm::vec3 p(from.x, *g, from.y);
                fmt::print("{}: start ({:.3f}, {:.3f}, {:.3f})\n", sphere ? "sphere" : "slices", p.x, p.y, p.z);
                // XI_MOVETEST_STEPS=1 prints every step.
                static const bool steps = std::getenv("XI_MOVETEST_STEPS") != nullptr;
                // XI_MOVETEST_FPS=N: steps of a frame at N fps (default 60).
                static const float stepDt = 1.0f / (std::getenv("XI_MOVETEST_FPS") ? std::stof(std::getenv("XI_MOVETEST_FPS")) : 60.0f);
                int               k     = 0;
                for (; k < 50000 && glm::distance(glm::vec2(p.x, p.z), to) > 0.05f; ++k)
                {
                    const glm::vec2 d = to - glm::vec2(p.x, p.z);
                    const glm::vec2 s = d * std::min(1.0f, 5.0f * stepDt / glm::length(d));
                    const glm::vec3 q = col.move(p, glm::vec3(s.x, 0.0f, s.y));
                    if (steps)
                    {
                        fmt::print("  ({:.3f}, {:.3f}, {:.3f})\n", q.x, q.y, q.z);
                    }
                    if (glm::distance(q, p) < 1e-4f)
                    {
                        break;
                    }
                    p = q;
                }
                fmt::print("  {} at ({:.3f}, {:.3f}, {:.3f}) after {} steps\n", glm::distance(glm::vec2(p.x, p.z), to) <= 0.05f ? "reached" : "blocked", p.x, p.y, p.z, k);
            }
        }
        else if (cmd == "walkcheck" && args.size() >= 2)
        {
            // Walking check (goal 11): characters dropped on random floor spots walk with random turns at run speed through
            // gfx::ZoneCollision::move; a step that crosses a wall triangle at 0.75 / 1.0 above the feet (the game's sphere band,
            // above the step-up) is a pass-through, one that ends with no floor under it or drops more than kMaxDrop is a fall.
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const int  agents = args.size() >= 3 ? std::stoi(args[2]) : 200;
            const int  steps  = args.size() >= 4 ? std::stoi(args[3]) : 600;
            const auto keys   = xi::dat::ZoneKeys::fromDll(idx.installDir() / "FFXiMain.dll");
            const auto zone   = xi::dat::loadZone(idx, xi::dat::zoneModelId(zoneId), keys);
            xi::gfx::ZoneCollision col;
            col.addCollision(zone.collision);
            for (const auto& b : zone.doorBoxes)
            {
                col.addDoorBox(b);
            }
            col.build();
            const auto& floors = col.floors().tris;
            if (floors.empty() || !col.hasCollision())
            {
                fmt::print("zone {}: no collision\n", zoneId);
                return 0;
            }
            // Walls whose top is within the step-up of the higher foot are steps (stepping down crosses their faces).
            float stepTop = 0.0f;
            auto crosses = [&](const glm::vec3& a, const glm::vec3& b) {
                const glm::vec3 d   = b - a;
                bool            hit = false;
                col.walls().visit(std::min(a.x, b.x), std::min(a.z, b.z), std::max(a.x, b.x), std::max(a.z, b.z), [&](const glm::vec3* v) {
                    const glm::vec3 e1 = v[1] - v[0], e2 = v[2] - v[0], pv = glm::cross(d, e2);
                    const float     det = glm::dot(e1, pv);
                    if (hit || std::abs(det) < 1e-9f || std::max({ v[0].y, v[1].y, v[2].y }) <= stepTop)
                    {
                        return;
                    }
                    const glm::vec3 tv = a - v[0];
                    const float     u  = glm::dot(tv, pv) / det;
                    const glm::vec3 qv = glm::cross(tv, e1);
                    const float     w  = glm::dot(d, qv) / det, t = glm::dot(e2, qv) / det;
                    hit                = u >= 0 && w >= 0 && u + w <= 1 && t > 0 && t < 1;
                    if (hit && std::getenv("XI_WALK_VERBOSE"))
                    {
                        const float lo = std::min({ v[0].y, v[1].y, v[2].y }), hi = std::max({ v[0].y, v[1].y, v[2].y });
                        const glm::vec3 n = glm::normalize(glm::cross(v[1] - v[0], v[2] - v[0]));
                        fmt::print("  crossing at game ({:.1f}, {:.1f}): wall y {:.2f}..{:.2f} over the feet, normal y {:.2f}, size {:.2f}\n", a.x, -a.z,
                                   lo - (stepTop - xi::gfx::ZoneCollision::kStepUp - 0.05f), hi - (stepTop - xi::gfx::ZoneCollision::kStepUp - 0.05f), n.y,
                                   glm::length(glm::cross(v[1] - v[0], v[2] - v[0])) * 0.5f);
                    }
                });
                return hit;
            };
            std::mt19937                           rng(12345 + zoneId);
            std::uniform_int_distribution<size_t> pick(0, floors.size() / 3 - 1);
            std::uniform_real_distribution<float>  turn(-0.6f, 0.6f), ang(0.0f, 6.2831853f);
            std::map<std::pair<int, int>, int>     through, fell;
            long                                   moved = 0, cycling = 0, cyclingFlat = 0;
            for (int a = 0; a < agents; ++a)
            {
                std::array<glm::vec3, 3> recent{}; // the last three positions: a step back onto one is a cycle (jitter)
                int                      seen = 0;
                const auto t = pick(rng);
                glm::vec3  feet = (floors[t * 3] + floors[t * 3 + 1] + floors[t * 3 + 2]) / 3.0f;
                const auto g    = col.groundHeight(feet.x, feet.z, feet.y + 0.05f);
                if (!g)
                {
                    continue;
                }
                feet.y        = *g;
                float heading = ang(rng);
                for (int k = 0; k < steps; ++k)
                {
                    heading += turn(rng) * 0.2f;
                    const glm::vec3 delta(std::cos(heading) * 5.0f / 60.0f, 0.0f, std::sin(heading) * 5.0f / 60.0f);
                    const glm::vec3 next = col.move(feet, delta);
                    if (glm::distance(next, feet) < 1e-4f)
                    {
                        heading = ang(rng); // blocked: try another way
                        continue;
                    }
                    ++moved;
                    // A visible jump (over 0.02) that lands within 0.02 of the spot 2 or 3 steps back: jitter, not a creep.
                    for (int r = 1; r < (glm::distance(next, feet) > 0.02f ? std::min(seen, 3) : 0); ++r)
                    {
                        if (glm::distance(next, recent[static_cast<std::size_t>((seen - 1 - r) % 3)]) < 0.02f)
                        {
                            ++cycling;
                            cyclingFlat += std::abs(next.y - feet.y) <= 0.01f;
                            if (std::getenv("XI_WALK_CYCLES") && cycling % 500 == 1) // render space, for xi-dat movetest
                            {
                                fmt::print("  cycle from ({:.3f}, {:.3f}, {:.3f}) heading {:.3f} to ({:.3f}, {:.3f}, {:.3f})\n", feet.x, feet.y, feet.z, heading,
                                           next.x, next.y, next.z);
                            }
                            break;
                        }
                    }
                    recent[static_cast<std::size_t>(seen++ % 3)] = next;
                    const std::pair<int, int> cell{ static_cast<int>(std::floor(next.x)), static_cast<int>(std::floor(-next.z)) };
                    stepTop = std::max(feet.y, next.y) + xi::gfx::ZoneCollision::kStepUp + 0.1f;
                    // A step off a ledge moves across at the old height and then falls straight down (the game has no drop
                    // limit): tested level, else the line from the lip down to the lower floor dips through the cliff face.
                    const glm::vec3 to = next.y < feet.y ? glm::vec3(next.x, feet.y, next.z) : next;
                    if (crosses(feet + glm::vec3(0, 0.75f, 0), to + glm::vec3(0, 0.75f, 0)) || crosses(feet + glm::vec3(0, 1.0f, 0), to + glm::vec3(0, 1.0f, 0)))
                    {
                        ++through[cell];
                        if (std::getenv("XI_WALK_VERBOSE")) // render space, for xi-dat movetest
                        {
                            fmt::print("  from ({:.3f}, {:.3f}, {:.3f}) to ({:.3f}, {:.3f}, {:.3f})\n", feet.x, feet.y, feet.z, next.x, next.y, next.z);
                        }
                    }
                    // A step onto a sliver between floor triangles stands on its steep face (move()'s seam fallback).
                    auto       under = col.groundHeight(next.x, next.z, next.y + 0.05f);
                    if (!under || std::abs(*under - next.y) > 0.05f)
                    {
                        if (const auto seam = col.seamHeight(next.x, next.z, next.y - 0.05f, next.y + 0.05f))
                        {
                            under = seam;
                        }
                    }
                    if (!under || std::abs(*under - next.y) > 0.05f || next.y < feet.y - xi::gfx::ZoneCollision::kMaxDrop)
                    {
                        ++fell[cell];
                    }
                    feet = next;
                }
            }
            auto total = [](const std::map<std::pair<int, int>, int>& m) {
                int n = 0;
                for (const auto& [c, k] : m) n += k;
                return n;
            };
            fmt::print("zone {}: {} jitter steps (a jump over 0.02 back to a spot 2 or 3 steps before; {} level)\n", zoneId, cycling, cyclingFlat);
            fmt::print("zone {}: {} agents, {} steps moved; pass-through {} steps at {} places; falls {} steps at {} places\n", zoneId, agents, moved,
                       total(through), through.size(), total(fell), fell.size());
            for (const auto* m : { &through, &fell })
            {
                std::vector<std::pair<int, std::pair<int, int>>> top;
                for (const auto& [c, k] : *m) top.push_back({ k, c });
                std::sort(top.rbegin(), top.rend());
                for (std::size_t i = 0; i < top.size() && i < 6; ++i)
                {
                    fmt::print("  {} at game ({}, {}): {} steps\n", m == &through ? "pass-through" : "fall", top[i].second.first, top[i].second.second, top[i].first);
                }
            }
        }
        else if (cmd == "weather" && args.size() >= 4)
        {
            const auto zoneId = static_cast<uint16_t>(std::stoul(args[1], nullptr, 0));
            const auto wid    = static_cast<uint16_t>(std::stoul(args[2], nullptr, 0));
            int        hh = 0, mm = 0;
            std::sscanf(args[3].c_str(), "%d:%d", &hh, &mm);
            const auto file = idx.read(xi::dat::zoneModelFileId(xi::dat::zoneModelId(zoneId)));
            const auto zw   = xi::dat::ZoneWeather::fromZoneFile(file);
            fmt::print("groups:");
            for (const auto& g : zw.groupNames())
            {
                fmt::print(" {}", g);
            }
            const auto r = zw.sample(wid, hh * 3600.0 + mm * 60.0);
            auto       c = [](const std::array<float, 3>& v) { return fmt::format("({:.0f},{:.0f},{:.0f})", v[0], v[1], v[2]); };
            fmt::print("\nflags {:#x}  sunZ {} x{:.2f}  moonZ {}  ambientZ {}  fog {} {:.1f}..{:.1f}\nclear {}  sky0 {}  sky7 {}\n", r.flags,
                       c(r.sunZ), r.multZ, c(r.moonZ), c(r.ambientZ), c(r.fogColor), r.fogStartZ, r.fogEndZ, c(r.clear), c(r.sky[0]), c(r.sky[7]));
        }
        else
        {
            fmt::print(stderr, "unknown command {}\n", cmd);
            return 2;
        }
    }
    catch (const std::exception& e)
    {
        fmt::print(stderr, "error: {}\n", e.what());
        return 1;
    }
    return 0;
}

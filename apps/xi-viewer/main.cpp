// SPDX-License-Identifier: GPL-3.0-or-later
// xi-viewer: Vulkan viewer for FFXI assets.
//
//   xi-viewer [--root DIR | $FFXI_ROOT] [--validation] [--hd-textures[=DIR]] [--frames N] [--screenshot OUT.png] [--size WxH] [--shots FILE] [--nosky] [--nearprobe D] MODE ARGS
//
// Modes:
//   textures FILEID   gallery of every texture in a DAT (wheel / PageUp / PageDown to scroll)
//   cubes FILEID      3D test scene: a grid of cubes textured with the DAT's textures
//   zone ZONEID       a zone's geometry, placed and textured (e.g. 230 = Southern San d'Oria)
//   skin FILEID       GPU skinning test: a two-bone column (textured from FILEID) bending at its joint
//   char RACE FACE HEAD BODY HANDS LEGS FEET [MAIN SUB RANGED]   a character (look ids as in S2C 0x00D)
//   npc MODELID       an NPC/monster model (S2C 0x00E look id); --model-fx also runs its own effects (`init`)
//
// --cam X,Y,Z,YAW,PITCH sets the start camera in game coordinates (Y down; angles in degrees, yaw 0 = north, 90 = east).
//
// 3D controls: WASD move, Q/E down/up, hold right mouse to look, Shift = fast. Esc quits.

#include <fstream>

#include "xi/install.h"
#include "dat/chunk.h"
#include "dat/dat_index.h"
#include "dat/texture.h"
#include "dat/hd_textures.h"
#include "dat/zone.h"
#include "gfx/camera.h"
#include "gfx/context.h"
#include "gfx/window.h"
#include "gfx/zone_scene.h"
#include "dat/character.h"
#include "gfx/character_model.h"
#include "client/effects.h"
#include "gfx/effect_renderer.h"
#include "gfx/environment.h"
#include "gfx/mesh_renderer.h"
#include "gfx/sky_renderer.h"
#include "gfx/skinned_renderer.h"
#include "gfx/pipeline.h"
#include "gfx/texture_set.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <optional>
#include <functional>
#include <unordered_map>
#include <string>
#include <vector>

using namespace xi;

namespace
{

struct Options
{
    std::string              root;
    bool                     validation = false;
    uint64_t                 maxFrames  = 0;
    std::string              screenshot;
    std::vector<std::string> args;
    std::string              cam;
    double                   time    = 12 * 3600.0; // Vana'diel time of day for zone mode (--time HH:MM)
    int                      weather = 1;           // weather id for zone mode (--weather ID)
    std::string              motion  = "idl";       // char/npc mode: motion stem (idl, wlk, run, or "none")
    std::string              display;               // --display primary|secondary|N (env XI_DISPLAY)
    float                    motionFrame = -1.0f;   // char/npc mode: fixed frame (screenshots); < 0 = animate
    float                    moveSpeed   = 0.0f;    // char/npc mode: walk the character forward at this speed (cloth sway)
    int                      width       = 1600;    // --size WxH
    int                      height      = 900;
    bool                     noSky       = false;   // --nosky: skip the sky and clear to magenta (void detection)
    bool                     modelFx     = false;   // --model-fx: npc mode also runs the model's own effects (`init`, generators)
    float                    nearProbe   = 0.0f;    // --nearprobe D: geometry farther than D yalms renders pure green
    struct Shot
    {
        std::string cam;
        double      time = 0;
        std::string out;
    };
    std::vector<Shot> shots; // --shots FILE: one "X,Y,Z,YAW,PITCH HH:MM OUT.png" per line, captured in one run
};

struct GalleryPush
{
    float    offset[2];
    float    size[2];
    uint32_t tex;
};

std::filesystem::path shaderDir()
{
    return gfx::shaderDir();
}

uint32_t loadTextures(const dat::DatIndex& index, uint32_t fileId, gfx::TextureSet& textures)
{
    const auto                  data = index.read(fileId);
    std::vector<dat::Chunk>     chunks;
    dat::parseChunks(data, chunks);
    uint32_t n = 0;
    for (const auto& c : chunks)
    {
        if (c.type == 0x20 || c.type == 0x1B)
        {
            try
            {
                textures.add(dat::parseTexture(c.payload));
                ++n;
            }
            catch (const std::exception& e)
            {
                spdlog::warn("texture {}: {}", c.name, e.what());
            }
        }
    }
    textures.commit();
    return n;
}

// Per-frame scene hook: input handling plus recording draws inside an active rendering pass.
struct Scene
{
    bool                                                        useDepth = false;
    std::function<void(const SDL_Event&)>                       onEvent;
    std::function<void(float dt)>                               update;
    std::function<void(VkCommandBuffer, VkExtent2D, uint32_t)>  render; // cmd, extent, frame slot
    std::function<std::string()>                                title;
    std::function<void(const Options::Shot&)>                   applyShot; // --shots: move the camera/clock
};

void runLoop(SDL_Window* window, gfx::Context& ctx, const Options& opt, Scene& scene)
{
    uint64_t totalFrames = 0;
    uint64_t maxFrames   = opt.maxFrames;
    if (!opt.screenshot.empty() && maxFrames == 0)
    {
        maxFrames = 3;
    }
    // A shot list renders a few frames per shot (geometry is resident, so the first frames are already complete).
    constexpr uint64_t framesPerShot = 3;
    if (!opt.shots.empty())
    {
        maxFrames = opt.shots.size() * framesPerShot;
    }
    bool     running  = true;
    uint64_t frames   = 0;
    auto     fpsStart = std::chrono::steady_clock::now();
    auto     last     = fpsStart;
    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_EVENT_QUIT || (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE))
            {
                running = false;
            }
            else if (scene.onEvent)
            {
                scene.onEvent(ev);
            }
        }
        if (!opt.shots.empty() && totalFrames % framesPerShot == 0 && scene.applyShot)
        {
            scene.applyShot(opt.shots[totalFrames / framesPerShot]);
        }
        const auto now = std::chrono::steady_clock::now();
        if (scene.update)
        {
            scene.update(std::chrono::duration<float>(now - last).count());
        }
        last = now;

        gfx::Context::Frame frame;
        if (!ctx.beginFrame(frame))
        {
            continue;
        }
        const auto ext = ctx.extent();
        auto       cmd = frame.cmd;
        gfx::Context::transition(cmd, frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        color.imageView        = frame.view;
        color.imageLayout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp           = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = { { 0.6f, 0.7f, 0.85f, 1.0f } }; // matches the placeholder fog colour
        if (opt.noSky)
        {
            color.clearValue.color = { { 1.0f, 0.0f, 1.0f, 1.0f } };
        }
        VkRenderingAttachmentInfo depth{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        depth.imageView               = ctx.depth().view;
        depth.imageLayout             = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp                  = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp                 = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = { 0.0f, 0 }; // reverse-Z: far = 0
        if (scene.useDepth)
        {
            gfx::Context::transition(cmd, ctx.depth().image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
        }
        VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        ri.renderArea           = { { 0, 0 }, ext };
        ri.layerCount           = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments    = &color;
        ri.pDepthAttachment     = scene.useDepth ? &depth : nullptr;
        vkCmdBeginRendering(cmd, &ri);
        VkViewport vp{ 0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1 };
        VkRect2D   sc{ { 0, 0 }, ext };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        scene.render(cmd, ext, frame.slot);
        vkCmdEndRendering(cmd);

        // Final frame of a --screenshot run: copy the swapchain image to a host buffer.
        // XI_VIEW_SEQ=N with --screenshot OUT.png: also capture every N-th frame as OUT_000.png, OUT_001.png ...
        static const uint64_t seqEvery = std::getenv("XI_VIEW_SEQ") ? std::strtoull(std::getenv("XI_VIEW_SEQ"), nullptr, 10) : 0;
        std::string           shotOut  = !opt.shots.empty() ? ((totalFrames + 1) % framesPerShot == 0 ? opt.shots[totalFrames / framesPerShot].out : std::string{})
                                                            : (totalFrames + 1 == maxFrames ? opt.screenshot : std::string{});
        if (shotOut.empty() && seqEvery > 0 && !opt.screenshot.empty() && (totalFrames + 1) % seqEvery == 0)
        {
            const auto dot = opt.screenshot.rfind('.');
            shotOut = fmt::format("{}_{:03d}{}", opt.screenshot.substr(0, dot), (totalFrames + 1) / seqEvery - 1, dot == std::string::npos ? std::string{} : opt.screenshot.substr(dot));
        }
        const bool        capture = !shotOut.empty();
        gfx::Buffer readback;
        if (capture)
        {
            readback = ctx.createBuffer(VkDeviceSize{ ext.width } * ext.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
            gfx::Context::transition(cmd, frame.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent      = { ext.width, ext.height, 1 };
            vkCmdCopyImageToBuffer(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
            gfx::Context::transition(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
        else
        {
            gfx::Context::transition(cmd, frame.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
        ctx.endFrame(frame);
        ++totalFrames;
        if (capture)
        {
            ctx.waitIdle();
            const bool bgr = ctx.colorFormat() == VK_FORMAT_B8G8R8A8_SRGB || ctx.colorFormat() == VK_FORMAT_B8G8R8A8_UNORM;
            std::vector<uint8_t> px(static_cast<const uint8_t*>(readback.mapped), static_cast<const uint8_t*>(readback.mapped) + readback.size);
            for (std::size_t i = 0; i < px.size(); i += 4)
            {
                if (bgr)
                {
                    std::swap(px[i], px[i + 2]);
                }
                px[i + 3] = 255;
            }
            dat::writePng(shotOut, ext.width, ext.height, px);
            ctx.destroyBuffer(readback);
            spdlog::info("screenshot written to {}", shotOut);
        }
        if (maxFrames && totalFrames >= maxFrames)
        {
            running = false;
        }

        ++frames;
        if (now - fpsStart >= std::chrono::seconds(2))
        {
            const double secs = std::chrono::duration<double>(now - fpsStart).count();
            SDL_SetWindowTitle(window, fmt::format("xi-viewer - {} - {:.0f} fps", scene.title ? scene.title() : "", static_cast<double>(frames) / secs).c_str());
            frames   = 0;
            fpsStart = now;
        }
    }
    ctx.waitIdle();
}

// WASD/QE + right-mouse look.
struct FlyController
{
    gfx::FlyCamera& cam;
    float           speed = 20.0f;
    bool            looking = false;

    void onEvent(SDL_Window* window, const SDL_Event& ev)
    {
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT)
        {
            looking = true;
            SDL_SetWindowRelativeMouseMode(window, true);
        }
        else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_RIGHT)
        {
            looking = false;
            SDL_SetWindowRelativeMouseMode(window, false);
        }
        else if (ev.type == SDL_EVENT_MOUSE_MOTION && looking)
        {
            cam.yaw += ev.motion.xrel * 0.003f;
            cam.pitch = std::clamp(cam.pitch - ev.motion.yrel * 0.003f, -1.55f, 1.55f);
        }
    }

    void update(float dt)
    {
        const bool* k     = SDL_GetKeyboardState(nullptr);
        const float v     = speed * dt * (k[SDL_SCANCODE_LSHIFT] ? 5.0f : 1.0f);
        glm::vec3   delta = {};
        delta += cam.forward() * (static_cast<float>(k[SDL_SCANCODE_W]) - static_cast<float>(k[SDL_SCANCODE_S]));
        delta += cam.right() * (static_cast<float>(k[SDL_SCANCODE_D]) - static_cast<float>(k[SDL_SCANCODE_A]));
        delta += glm::vec3(0, 1, 0) * (static_cast<float>(k[SDL_SCANCODE_E]) - static_cast<float>(k[SDL_SCANCODE_Q]));
        cam.position += delta * v;
    }
};

// Unit cube with per-face UVs, as 24 vertices / 36 indices.
void appendCube(std::vector<gfx::Vertex>& v, std::vector<uint32_t>& idx)
{
    const glm::vec3 n[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (const auto& normal : n)
    {
        const glm::vec3 u    = std::abs(normal.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 t    = glm::cross(normal, u);
        const auto      base = static_cast<uint32_t>(v.size());
        const glm::vec2 uvs[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (int i = 0; i < 4; ++i)
        {
            const float s = (i == 1 || i == 2) ? 1.0f : -1.0f;
            const float r = (i >= 2) ? 1.0f : -1.0f;
            v.push_back({ (normal + t * s + u * r) * 0.5f, normal, 0x80808080u, uvs[i] });
        }
        idx.insert(idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (const char* lvl = std::getenv("XI_LOG"))
    {
        spdlog::set_level(spdlog::level::from_str(lvl)); // e.g. XI_LOG=debug
    }
    Options opt;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--root" && i + 1 < argc) opt.root = argv[++i];
        else if (a == "--validation") opt.validation = true;
        else if (a == "--hd-textures") xi::dat::hd::setRuntimeDir(xi::dat::hd::defaultDir(), false); // goal 41
        else if (a.rfind("--hd-textures=", 0) == 0) xi::dat::hd::setRuntimeDir(std::filesystem::path(a.substr(14)), false);
        else if (a == "--frames" && i + 1 < argc) opt.maxFrames = std::stoull(argv[++i]);
        else if (a == "--screenshot" && i + 1 < argc) opt.screenshot = argv[++i];
        else if (a == "--cam" && i + 1 < argc) opt.cam = argv[++i];
        else if (a == "--time" && i + 1 < argc)
        {
            int hh = 0, mm = 0;
            std::sscanf(argv[++i], "%d:%d", &hh, &mm);
            opt.time = hh * 3600.0 + mm * 60.0;
        }
        else if (a == "--weather" && i + 1 < argc) opt.weather = std::stoi(argv[++i]);
        else if (a == "--motion" && i + 1 < argc) opt.motion = argv[++i];
        else if (a == "--display" && i + 1 < argc) opt.display = argv[++i];
        else if (a == "--frame" && i + 1 < argc) opt.motionFrame = std::stof(argv[++i]);
        else if (a == "--move" && i + 1 < argc) opt.moveSpeed = std::stof(argv[++i]);
        else if (a == "--nosky") opt.noSky = true;
        else if (a == "--model-fx") opt.modelFx = true;
        else if (a == "--nearprobe" && i + 1 < argc) opt.nearProbe = std::stof(argv[++i]);
        else if (a == "--shots" && i + 1 < argc)
        {
            std::ifstream in(argv[++i]);
            std::string   line;
            while (std::getline(in, line))
            {
                char cam[128]{}, out[1024]{};
                int  hh = 0, mm = 0;
                if (std::sscanf(line.c_str(), "%127s %d:%d %1023[^\n]", cam, &hh, &mm, out) == 4)
                {
                    opt.shots.push_back({ cam, hh * 3600.0 + mm * 60.0, out });
                }
            }
        }
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &opt.width, &opt.height);
        else opt.args.push_back(a);
    }
    if (opt.display.empty() && std::getenv("XI_DISPLAY"))
    {
        opt.display = std::getenv("XI_DISPLAY");
    }
    if (opt.root.empty() && xi::install::gameRootOrNull())
    {
        opt.root = xi::install::gameRootOrNull(); // FFXI_ROOT, else our imported copy, else ~/.wine
    }
    if (opt.root.empty() || opt.args.size() < 2)
    {
        fmt::print(stderr, "usage: xi-viewer [--root DIR | $FFXI_ROOT] [--validation] [--frames N] [--screenshot OUT.png] [--size WxH] [--shots FILE] [--nosky] [--nearprobe D] (textures FILEID | cubes FILEID | zone ZONEID | skin FILEID | char RACE FACE H B HA L F | npc MODELID)\n");
        return 2;
    }
    const auto& mode   = opt.args[0];
    const auto  fileId = static_cast<uint32_t>(std::stoul(opt.args[1], nullptr, 0));

    try
    {
        const dat::DatIndex index(opt.root);
        gfx::prepareVideoDriver(opt.display);
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            throw std::runtime_error(fmt::format("SDL_Init: {}", SDL_GetError()));
        }
        SDL_Window* window = gfx::createWindow("xi-viewer", opt.width, opt.height, opt.display, /*takeFocus=*/opt.screenshot.empty());
        {
            gfx::Context    ctx(window, opt.validation);
            gfx::TextureSet textures(ctx);
            const auto      t0 = std::chrono::steady_clock::now();
            const auto      n  = (mode == "char" || mode == "npc") ? 0u
                                                                     : loadTextures(index, mode == "zone" ? dat::zoneModelFileId(dat::zoneModelId(static_cast<uint16_t>(fileId))) : fileId, textures);
            spdlog::info("uploaded {} textures in {} ms", n, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());

            if (mode == "textures")
            {
                VkPushConstantRange        pcr{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GalleryPush) };
                auto                       setLayout = textures.layout();
                VkPipelineLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
                lci.setLayoutCount         = 1;
                lci.pSetLayouts            = &setLayout;
                lci.pushConstantRangeCount = 1;
                lci.pPushConstantRanges    = &pcr;
                VkPipelineLayout layout    = VK_NULL_HANDLE;
                VK_CHECK(vkCreatePipelineLayout(ctx.device(), &lci, nullptr, &layout));
                gfx::PipelineDesc pd;
                pd.vert             = gfx::loadShader(ctx.device(), shaderDir() / "gallery.vert.spv");
                pd.frag             = gfx::loadShader(ctx.device(), shaderDir() / "gallery.frag.spv");
                pd.layout           = layout;
                pd.colorFormat      = ctx.colorFormat();
                VkPipeline pipeline = gfx::createPipeline(ctx.device(), pd);

                float scroll = 0.0f;
                Scene scene;
                scene.onEvent = [&](const SDL_Event& ev) {
                    if (ev.type == SDL_EVENT_MOUSE_WHEEL) scroll = std::max(0.0f, scroll - ev.wheel.y * 120.0f);
                    else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_PAGEDOWN) scroll += 600.0f;
                    else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_PAGEUP) scroll = std::max(0.0f, scroll - 600.0f);
                };
                scene.title  = [&] { return fmt::format("file {} - {} textures", fileId, textures.size()); };
                scene.render = [&](VkCommandBuffer cmd, VkExtent2D ext, uint32_t) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    auto set = textures.set();
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
                    const float pad = 8.0f;
                    float       x = pad, y = pad - scroll, rowH = 0.0f;
                    const auto  W = static_cast<float>(ext.width), H = static_cast<float>(ext.height);
                    for (uint32_t i = 0; i < textures.size(); ++i)
                    {
                        const auto  e = textures.extent(i);
                        const float w = std::max(128.0f, static_cast<float>(e.width));
                        const float h = w * static_cast<float>(e.height) / static_cast<float>(e.width);
                        if (x + w > W && x > pad)
                        {
                            x = pad;
                            y += rowH + pad;
                            rowH = 0.0f;
                        }
                        GalleryPush p{ { x / W * 2.0f - 1.0f, y / H * 2.0f - 1.0f }, { w / W * 2.0f, h / H * 2.0f }, i };
                        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(p), &p);
                        vkCmdDraw(cmd, 6, 1, 0, 0);
                        x += w + pad;
                        rowH = std::max(rowH, h);
                    }
                };
                runLoop(window, ctx, opt, scene);
                vkDestroyPipeline(ctx.device(), pipeline, nullptr);
                vkDestroyShaderModule(ctx.device(), pd.vert, nullptr);
                vkDestroyShaderModule(ctx.device(), pd.frag, nullptr);
                vkDestroyPipelineLayout(ctx.device(), layout, nullptr);
            }
            else if (mode == "cubes")
            {
                gfx::MeshRenderer        meshes(ctx, textures);
                std::vector<gfx::Vertex> v;
                std::vector<uint32_t>    idx;
                appendCube(v, idx);
                // One mesh per texture so each cube shows a different one.
                std::vector<uint32_t> ids;
                for (uint32_t t = 0; t < textures.size(); ++t)
                {
                    ids.push_back(meshes.addMesh(v, idx, { { 0, static_cast<uint32_t>(idx.size()), 0, t, false } }));
                }
                for (int z = 0; z < 40; ++z)
                {
                    for (int x = 0; x < 40; ++x)
                    {
                        const auto m = glm::scale(glm::translate(glm::mat4(1), glm::vec3(static_cast<float>(x) * 6.0f - 120.0f, 0.0f, static_cast<float>(z) * -6.0f)), glm::vec3(4.0f));
                        meshes.addInstance(ids[static_cast<std::size_t>(z * 40 + x) % ids.size()], m);
                    }
                }
                meshes.upload();

                gfx::FlyCamera cam;
                cam.position = { 0.0f, 25.0f, 30.0f };
                cam.pitch    = -0.45f;
                FlyController fly{ cam };
                Scene         scene;
                scene.useDepth = true;
                scene.onEvent  = [&](const SDL_Event& ev) { fly.onEvent(window, ev); };
                scene.update   = [&](float dt) { fly.update(dt); };
                scene.title    = [&] { return fmt::format("cubes - {} draws, {} tris", meshes.stats().draws, meshes.stats().triangles); };
                scene.render   = [&](VkCommandBuffer cmd, VkExtent2D ext, uint32_t slot) {
                    const float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);
                    gfx::SceneParams sp;
                    sp.eye = cam.position;
                    meshes.draw(cmd, slot, cam.projection(aspect) * cam.view(), sp);
                };
                runLoop(window, ctx, opt, scene);
            }
            else if (mode == "char" || mode == "npc")
            {
                std::optional<dat::CharacterAsset> asset;
                auto num = [&](std::size_t i) { return opt.args.size() > i ? static_cast<uint16_t>(std::stoul(opt.args[i], nullptr, 0)) : uint16_t{ 0 }; };
                if (mode == "char")
                {
                    asset = dat::loadPcAsset(index, static_cast<uint8_t>(num(1)), static_cast<uint8_t>(num(2)),
                                             { num(3), num(4), num(5), num(6), num(7), num(8), num(9), num(10) });
                }
                else
                {
                    asset = dat::loadNpcAsset(index, num(1));
                }
                if (!asset)
                {
                    throw std::runtime_error("could not load that model");
                }
                spdlog::info("model: {} bones, {} triangles, {} parts, {} textures", asset->skeleton.bones.size(), asset->vertices.size() / 3,
                             asset->parts.size(), asset->textures.size());
                gfx::TextureSet      charTextures(ctx);
                gfx::SkinnedRenderer skinned(ctx, charTextures);
                auto                 cm = gfx::uploadCharacter(*asset, charTextures, skinned);
                // Weapon-class battle files first (PCs, §10.6), then the base motion files.
                std::vector<uint32_t> motionFiles = asset->battleMotionFiles;
                motionFiles.insert(motionFiles.end(), asset->motionFiles.begin(), asset->motionFiles.end());
                spdlog::info("weapon class {}", asset->weaponClass);
                // "--motion emote:N": emote N's motion (dat::loadEmoteMotion) for the character's race.
                const bool           emote  = opt.motion.rfind("emote:", 0) == 0 && mode == "char";
                const dat::MotionSet motion = opt.motion == "none" ? dat::MotionSet{}
                                            : emote ? dat::loadEmoteMotion(index, static_cast<uint8_t>(std::stoi(opt.args.at(1))), static_cast<uint8_t>(std::stoi(opt.motion.substr(6))))
                                                    : dat::loadMotionSet(index, motionFiles, opt.motion);
                spdlog::info("motion '{}': {} pieces, {} frames, speed {}", opt.motion, motion.pieces.size(), motion.frames(), motion.speed());
                // A battle stance holds the main weapon in the grip (dat::drawnWeaponBone).
                if (opt.motion == "btl" || opt.motion.rfind("at", 0) == 0)
                {
                    cm.weaponBone      = asset->mainWeaponBone;
                    cm.drawnWeaponBone = dat::drawnWeaponBone(asset->skeleton, dat::loadMotionSet(index, motionFiles, "btl"), asset->mainWeaponBone);
                    spdlog::info("main weapon bone {}, drawn bone {}", cm.weaponBone, cm.drawnWeaponBone);
                }
                const auto           animStart = std::chrono::steady_clock::now();
                gfx::FlyCamera       cam;
                cam.position = { 0.0f, cm.height * 0.55f, cm.height * 1.6f };
                if (!opt.cam.empty())
                {
                    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
                    std::sscanf(opt.cam.c_str(), "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch);
                    cam.position = { x, -y, -z }; // game -> render: (x, -y, -z)
                    cam.yaw      = glm::radians(yaw);
                    cam.pitch    = glm::radians(pitch);
                }
                // --model-fx: the model's own effects, as xi-client runs them when the actor appears (`init`, then the
                // model generators; client::ActionEffects).
                std::optional<client::EffectSystem> fx;
                std::optional<gfx::EffectRenderer>  fxRenderer;
                std::vector<client::EffectSystem::Batch> fxBatches;
                std::vector<uint32_t>                    fxFiles = asset->effectFiles;
                fxFiles.insert(fxFiles.end(), motionFiles.begin(), motionFiles.end());
                if (opt.modelFx)
                {
                    fx.emplace(index);
                    fxRenderer.emplace(ctx);
                    fx->setActorFiles([&fxFiles](uint32_t id) { return id == 1 ? fxFiles : std::vector<uint32_t>{}; });
                }
                FlyController fly{ cam };
                fly.speed = 2.0f;
                Scene scene;
                scene.useDepth = true;
                scene.onEvent  = [&](const SDL_Event& ev) { fly.onEvent(window, ev); };
                scene.update   = [&](float dt) { fly.update(dt); };
                scene.title    = [&] { return fmt::format("{} - {} triangles", mode, asset->vertices.size() / 3); };
                std::vector<glm::mat4> palette;
                gfx::ClothSim          cloth;
                float                  simSecs = 0.0f, lastSecs = 0.0f;
                spdlog::info("cloth: {} meshes, {} particles (palette base {})", cm.cloth.meshes.size(), cm.cloth.particles, cm.cloth.base);
                scene.render = [&](VkCommandBuffer cmd, VkExtent2D ext, uint32_t slot) {
                    // Face the camera: heading 64 = facing game -z = render +z (toward the default camera). Screenshot runs
                    // use a fixed 60 fps clock so captures repeat.
                    const float wall = std::chrono::duration<float>(std::chrono::steady_clock::now() - animStart).count();
                    const float dt   = opt.screenshot.empty() ? wall - lastSecs : 1.0f / 60.0f;
                    lastSecs         = wall;
                    simSecs += dt;
                    const float secs  = simSecs;
                    const float frame = opt.motionFrame >= 0.0f ? opt.motionFrame : secs * 30.0f * motion.speed(); // 30 ticks/s (INFERRED)
                    // --move: walk toward the camera's right (render +x = heading 0), the camera following.
                    const glm::vec3 at(opt.moveSpeed * secs, 0.0f, 0.0f);
                    if (opt.moveSpeed != 0.0f)
                    {
                        cam.position.x += opt.moveSpeed * dt;
                    }
                    gfx::characterPalette(cm, gfx::ZoneScene::renderToGame(at), static_cast<uint8_t>(opt.moveSpeed != 0.0f ? 0 : 64), palette,
                                          gfx::AnimState{ motion.empty() ? nullptr : &motion, frame });
                    gfx::holdWeapon(cm, palette);
                    if (std::getenv("XI_NO_CLOTH") == nullptr)
                    {
                        gfx::simulateCloth(cm.cloth, cloth, palette, at, dt);
                    }
                    const float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);
                    gfx::SceneParams sp;
                    sp.eye         = cam.position;
                    sp.camRight = cam.right(), sp.camForward = cam.forward(), sp.camUp = glm::normalize(glm::cross(cam.right(), cam.forward()));
                    sp.ambient     = glm::vec3(0.6f);
                    sp.light0Dir   = glm::normalize(glm::vec3(0.3f, 0.8f, 0.6f));
                    sp.light0Color = glm::vec3(0.6f);
                    sp.fogStart    = 1000.0f;
                    sp.fogEnd      = 2000.0f;
                    skinned.draw(cmd, slot, cam.projection(aspect) * cam.view(), sp, { gfx::SkinnedRenderer::Instance{ cm.model, palette } });
                    if (fx)
                    {
                        // The actor (id 1) at `at`, its skeleton points from this frame's palette (as xi-client's
                        // ActorPoses::point), in game space.
                        const client::FxActorLookup lookup = [&](uint32_t id) -> std::optional<client::FxActor> {
                            if (id != 1)
                            {
                                return std::nullopt;
                            }
                            client::FxActor a{ gfx::ZoneScene::renderToGame(at), -64.0f * 3.14159265f / 128.0f, cm.height, {}, client::fxActorSize(cm.skeleton.bounds[0], cm.scale) };
                            a.point = [&](uint32_t pt) -> std::optional<glm::vec3> {
                                const auto& pts = cm.skeleton.points;
                                if (pt >= pts.size() || pts[pt].bone < 0 || static_cast<std::size_t>(pts[pt].bone) >= palette.size())
                                {
                                    return std::nullopt;
                                }
                                const auto& o = pts[pt].offset;
                                return gfx::ZoneScene::renderToGame(glm::vec3(palette[static_cast<std::size_t>(pts[pt].bone)] * glm::vec4(o[0], o[1], o[2], 1.0f)));
                            };
                            return a;
                        };
                        static bool started = false;
                        if (!std::exchange(started, true))
                        {
                            const bool played = fx->playFromActor(1, dat::fxName("init"), 1, 1);
                            fx->startModelGenerators(1);
                            spdlog::info("model fx: init {}", played);
                        }
                        fx->update(dt, lookup);
                        fxBatches.clear();
                        fx->build(fxBatches, gfx::ZoneScene::renderToGame(cam.right()), gfx::ZoneScene::renderToGame(-sp.camUp));
                        fxRenderer->prepare(fxBatches);
                        fxRenderer->beginFrame(slot);
                        fxRenderer->draw(cmd, slot, cam.projection(aspect) * cam.view(), fxBatches);
                    }
                };
                runLoop(window, ctx, opt, scene);
            }
            else if (mode == "skin")
            {
                gfx::SkinnedRenderer skinned(ctx, textures);
                // Column 1 x 4 x 1, 8 rings; vertices weighted between bone 0 (base) and bone 1 (top half).
                std::vector<gfx::SkinnedVertex> v;
                std::vector<uint32_t>            idx;
                const int                        rings = 9;
                for (int r = 0; r < rings; ++r)
                {
                    const float y  = static_cast<float>(r) * 0.5f;
                    const float w1 = std::clamp((y - 1.5f) / 1.0f, 0.0f, 1.0f); // blend across the joint at y = 2
                    const glm::vec2 corners[4] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
                    for (int c = 0; c < 4; ++c)
                    {
                        gfx::SkinnedVertex sv{};
                        // Pre-weighted like FFXI's 2-bone vertices: p0/n0 carry (1 - w1), p1/n1 carry w1.
                        const glm::vec3 pos(corners[c].x, y, corners[c].y);
                        const glm::vec3 nrm = glm::normalize(glm::vec3(corners[c].x, 0, corners[c].y));
                        sv.p0       = pos * (1.0f - w1);
                        sv.p1       = pos * w1;
                        sv.n0       = nrm * (1.0f - w1);
                        sv.n1       = nrm * w1;
                        sv.uv       = { static_cast<float>(c) * 0.25f, y / 4.0f };
                        sv.bones[0] = 0;
                        sv.bones[1] = 1;
                        sv.weight1  = w1;
                        v.push_back(sv);
                    }
                }
                for (int r = 0; r + 1 < rings; ++r)
                {
                    for (int c = 0; c < 4; ++c)
                    {
                        const uint32_t a = static_cast<uint32_t>(r * 4 + c), b = static_cast<uint32_t>(r * 4 + (c + 1) % 4);
                        // clockwise front (matches the FFXI pipelines)
                        idx.insert(idx.end(), { a, a + 4, b, b, a + 4, b + 4 });
                    }
                }
                const uint32_t model = skinned.addModel(v, idx, { { 0, static_cast<uint32_t>(idx.size()), 0, 0, false, true, false } });

                gfx::FlyCamera cam;
                cam.position = { 0.0f, 2.5f, 8.0f };
                FlyController fly{ cam };
                Scene         scene;
                scene.useDepth = true;
                scene.onEvent  = [&](const SDL_Event& ev) { fly.onEvent(window, ev); };
                scene.update   = [&](float dt) { fly.update(dt); };
                scene.title    = [&] { return std::string("skinning test"); };
                const auto t0s = std::chrono::steady_clock::now();
                scene.render   = [&](VkCommandBuffer cmd, VkExtent2D ext, uint32_t slot) {
                    const float t     = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0s).count();
                    const float angle = opt.screenshot.empty() ? std::sin(t * 1.5f) * 0.9f : 0.9f;
                    // Bone 1 bends about the joint at y = 2 (bone matrices map bind-pose model space to world).
                    const glm::mat4 bend = glm::translate(glm::mat4(1), glm::vec3(0, 2, 0)) * glm::rotate(glm::mat4(1), angle, glm::vec3(0, 0, 1)) *
                                           glm::translate(glm::mat4(1), glm::vec3(0, -2, 0));
                    gfx::SkinnedRenderer::Instance inst{ model, { glm::mat4(1), bend } };
                    const float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);
                    gfx::SceneParams sp;
                    sp.eye = cam.position;
                    sp.camRight = cam.right(), sp.camForward = cam.forward(), sp.camUp = glm::normalize(glm::cross(cam.right(), cam.forward()));
                    skinned.draw(cmd, slot, cam.projection(aspect) * cam.view(), sp, { inst });
                };
                runLoop(window, ctx, opt, scene);
            }
            else if (mode == "zone")
            {
                const auto zoneId = static_cast<uint16_t>(fileId);
                const auto t0z    = std::chrono::steady_clock::now();
                const auto keys   = dat::ZoneKeys::fromDll(index.installDir() / "FFXiMain.dll");
                const auto zone   = dat::loadZone(index, dat::zoneModelId(zoneId), keys);
                const auto zoneWeather = dat::ZoneWeather::fromZoneFile(zone.file);
                gfx::SkyRenderer skyRenderer(ctx);

                // Texture lookup by the 16-char name; untextured / missing pieces use a white texture.
                std::unordered_map<std::string, uint32_t> texByName;
                {
                    const auto              file = zone.file;
                    std::vector<dat::Chunk> chunks;
                    dat::parseChunks(file, chunks);
                    uint32_t ti = 0;
                    for (const auto& c : chunks)
                    {
                        if (c.type == 0x20 || c.type == 0x1B)
                        {
                            try
                            {
                                const auto tex = dat::parseTexture(c.payload);
                                texByName.try_emplace(tex.name, ti++);
                            }
                            catch (const std::exception&)
                            {
                            }
                        }
                    }
                }
                dat::Texture white;
                white.name   = "white";
                white.width  = 4;
                white.height = 4;
                white.data.assign(4 * 4 * 4, std::byte{ 0x80 }); // 0x80 = full intensity / opaque in FFXI terms
                const uint32_t whiteTex = textures.add(white);
                textures.commit();

                gfx::MeshRenderer                         meshes(ctx, textures);
                std::unordered_map<std::string, uint32_t> meshByName;
                uint32_t                                  missingTex = 0;
                for (const auto& [name, model] : zone.models)
                {
                    std::vector<gfx::Vertex> verts;
                    std::vector<uint32_t>    idx;
                    std::vector<gfx::SubMesh> subs;
                    for (const auto& pc : model.pieces)
                    {
                        if (pc.triangles.empty())
                        {
                            continue;
                        }
                        const auto base = static_cast<uint32_t>(verts.size());
                        for (const auto& v : pc.vertices)
                        {
                            // D3DCOLOR bytes are B,G,R,A; the GPU attribute wants R,G,B,A.
                            const uint32_t c    = v.color;
                            const uint32_t rgba = ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16) | (c & 0xFF000000u);
                            verts.push_back({ { v.pos[0], v.pos[1], v.pos[2] }, { v.normal[0], v.normal[1], v.normal[2] }, rgba, { v.uv[0], v.uv[1] } });
                        }
                        gfx::SubMesh sm;
                        sm.firstIndex = static_cast<uint32_t>(idx.size());
                        for (const auto t : pc.triangles)
                        {
                            idx.push_back(base + t);
                        }
                        sm.indexCount = static_cast<uint32_t>(pc.triangles.size());
                        const auto it = texByName.find(pc.texture);
                        if (it != texByName.end())
                        {
                            sm.texture = it->second;
                        }
                        else
                        {
                            sm.texture = whiteTex;
                            missingTex += pc.texture.find_first_not_of(' ') != std::string::npos;
                        }
                        // Client render states (docs/dat-format.md §6): bit 31 blend, bit 29 double-sided,
                        // alpha test only for model names starting with '_'.
                        sm.blended     = (pc.flags & 0x8) != 0;
                        sm.doubleSided = (pc.flags & 0x2) != 0;
                        sm.alphaTest   = !name.empty() && name[0] == '_';
                        subs.push_back(sm);
                    }
                    if (!subs.empty())
                    {
                        meshByName[name] = meshes.addMesh(verts, idx, std::move(subs));
                    }
                }

                // Game space is D3D left-handed with Y down; reflecting Y gives right-handed Y-up and turns the
                // clockwise front faces counter-clockwise (docs/dat-format.md §5.5).
                // Game space (+x east, +y down, +z north) -> render (Y up): 180-degree rotation about X.
                const glm::mat4 flipY = glm::mat4(glm::vec4(1, 0, 0, 0), glm::vec4(0, -1, 0, 0), glm::vec4(0, 0, -1, 0), glm::vec4(0, 0, 0, 1));
                for (const auto& pl : zone.placements)
                {
                    const auto it = meshByName.find(pl.name);
                    if (it == meshByName.end())
                    {
                        continue;
                    }
                    // Client: W = S * Rx * Ry * Rz * T with row vectors == T * Rz * Ry * Rx * S with column vectors.
                    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(pl.pos[0], pl.pos[1], pl.pos[2]));
                    m           = glm::rotate(m, pl.rot[2], glm::vec3(0, 0, 1));
                    m           = glm::rotate(m, pl.rot[1], glm::vec3(0, 1, 0));
                    m           = glm::rotate(m, pl.rot[0], glm::vec3(1, 0, 0));
                    m           = glm::scale(m, glm::vec3(pl.scale[0], pl.scale[1], pl.scale[2]));
                    // Mirrored when the game-space transform flips handedness (client: scale.x * scale.z < 0).
                    meshes.addInstance(it->second, flipY * m, glm::determinant(glm::mat3(m)) < 0.0f);
                }
                meshes.upload();
                spdlog::info("zone {}: {} meshes, {} placements, {} draws, {} triangles, {} missing textures, ready in {} ms", zoneId, meshByName.size(),
                             zone.placements.size(), meshes.stats().draws, meshes.stats().triangles, missingTex,
                             std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0z).count());

                gfx::FlyCamera cam;
                cam.position = { 0.0f, 30.0f, 0.0f };
                cam.pitch    = -0.3f;
                const auto setCam = [&](const std::string& spec) {
                    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
                    std::sscanf(spec.c_str(), "%f,%f,%f,%f,%f", &x, &y, &z, &yaw, &pitch);
                    cam.position = { x, -y, -z }; // game -> render: (x, -y, -z)
                    cam.yaw      = glm::radians(yaw);
                    cam.pitch    = glm::radians(pitch);
                };
                if (!opt.cam.empty())
                {
                    setCam(opt.cam);
                }
                double time = opt.time;
                FlyController fly{ cam };
                Scene         scene;
                scene.useDepth = true;
                scene.onEvent  = [&](const SDL_Event& ev) { fly.onEvent(window, ev); };
                scene.update   = [&](float dt) { fly.update(dt); };
                scene.applyShot = [&](const Options::Shot& shot) {
                    setCam(shot.cam);
                    time = shot.time;
                };
                scene.title    = [&] {
                    return fmt::format("zone {} - {} draws, {} tris - game pos ({:.1f}, {:.1f}, {:.1f})", zoneId, meshes.stats().draws, meshes.stats().triangles,
                                       cam.position.x, -cam.position.y, -cam.position.z);
                };
                scene.render = [&](VkCommandBuffer cmd, VkExtent2D ext, uint32_t slot) {
                    const float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);
                    gfx::SceneParams           sp;
                    gfx::SkyRenderer::Gradient sky;
                    sp.eye = cam.position;
                    gfx::applyEnvironment(zoneWeather.sample(static_cast<uint16_t>(opt.weather), time), time, sp, sky);
                    if (opt.nearProbe > 0.0f)
                    {
                        sp.fogColor = { 0.0f, 1.0f, 0.0f };
                        sp.fogStart = opt.nearProbe - 0.5f;
                        sp.fogEnd   = opt.nearProbe;
                    }
                    if (!opt.noSky)
                    {
                        skyRenderer.draw(cmd, slot, cam.forward(), cam.right(), glm::normalize(glm::cross(cam.right(), cam.forward())), cam.fovY, aspect, sky);
                    }
                    meshes.draw(cmd, slot, cam.projection(aspect) * cam.view(), sp);
                };
                runLoop(window, ctx, opt, scene);
            }
            else
            {
                spdlog::error("unknown mode {}", mode);
            }
        }
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    catch (const std::exception& e)
    {
        spdlog::error("{}", e.what());
        return 1;
    }
    return 0;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #139: FSR 3.1 on the real GPU, without logging in: the upscaler's context, its shared resources and a few
// dispatches over a synthetic frame, with the validation layer on. It caught nothing in CI's GPU-less runners, so it
// skips there (no display / no Vulkan device: exit 77); on a machine with a GPU it runs in under a second.
#include "gfx/context.h"
#include "gfx/fsr.h"

#include <SDL3/SDL.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>

namespace
{

// Counts the validation layer's errors (gfx::Context logs them as "vulkan: ..." at error level).
std::atomic<int> g_errors{ 0 };
class CountErrors : public spdlog::sinks::base_sink<std::mutex>
{
protected:
    void sink_it_(const spdlog::details::log_msg& m) override
    {
        if (m.level >= spdlog::level::err)
        {
            ++g_errors;
            std::fprintf(stderr, "%.*s\n", static_cast<int>(m.payload.size()), m.payload.data());
        }
    }
    void flush_() override {}
};

} // namespace

int main()
{
    if (!xi::gfx::fsrBuilt())
    {
        std::puts("fsr_gpu: built without FSR; skipped");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::printf("fsr_gpu: no display (%s); skipped\n", SDL_GetError());
        return 77;
    }
    SDL_Window* window = SDL_CreateWindow("fsr_gpu", 320, 200, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window)
    {
        std::printf("fsr_gpu: no window (%s); skipped\n", SDL_GetError());
        return 77;
    }
    spdlog::default_logger()->sinks().push_back(std::make_shared<CountErrors>());
    int rc = 0;
    try
    {
        xi::gfx::Context ctx(window, true);
        if (!ctx.fsr())
        {
            std::printf("fsr_gpu: this GPU can't run FSR (%s); skipped\n", ctx.fsrMissing().c_str());
            rc = 77;
        }
        else
        {
            using xi::gfx::Image;
            constexpr VkExtent2D out{ 640, 360 };
            xi::gfx::Fsr         fsr(ctx, xi::gfx::FsrMode::Quality);
            const VkExtent2D     render = fsr.renderExtent(out);
            // A mip chain with only mip 0 in the input layout, as the client's scene colour (SceneTarget) has.
            Image color  = ctx.createImage(render, VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                           VK_IMAGE_ASPECT_COLOR_BIT, 9);
            Image depth  = ctx.createImage(render, VK_FORMAT_D32_SFLOAT,
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                           VK_IMAGE_ASPECT_DEPTH_BIT);
            Image motion = ctx.createImage(render, VK_FORMAT_R16G16_SFLOAT,
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
            Image output = ctx.createImage(out, VK_FORMAT_R16G16B16A16_SFLOAT,
                                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
            bool  ok     = fsr.ok();
            for (int frame = 0; frame < 4 && ok; ++frame)
            {
                ctx.immediateSubmit([&](VkCommandBuffer cmd) {
                    using xi::gfx::Context;
                    constexpr auto RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    Context::transition(cmd, color.image, VK_IMAGE_LAYOUT_UNDEFINED, RO, VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                        1);
                    Context::transition(cmd, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, RO, VK_IMAGE_ASPECT_DEPTH_BIT);
                    Context::transition(cmd, motion.image, VK_IMAGE_LAYOUT_UNDEFINED, RO);
                    Context::transition(cmd, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                    xi::gfx::UpscaleInputs in;
                    in.color  = &color;
                    in.depth  = &depth;
                    in.motion = &motion;
                    in.output = &output;
                    in.render = render;
                    in.jitter = { 0.25f * static_cast<float>(frame % 2 ? 1 : -1), 0.1f };
                    in.reset  = frame == 0;
                    in.fovY   = 1.0f;
                    ok        = fsr.evaluate(cmd, in);
                });
            }
            ctx.waitIdle();
            for (Image* i : { &color, &depth, &motion, &output })
            {
                ctx.destroyImage(*i);
            }
            std::printf("fsr_gpu: %s, %d validation error(s)\n", ok ? "4 dispatches recorded and run" : "FSR failed",
                        g_errors.load());
            rc = ok && g_errors.load() == 0 ? 0 : 1;
        }
    }
    catch (const std::exception& e)
    {
        std::printf("fsr_gpu: no usable Vulkan device (%s); skipped\n", e.what());
        rc = 77;
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return rc;
}

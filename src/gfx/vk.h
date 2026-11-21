// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vulkan/vulkan.h>

#include <fmt/format.h>

#include <stdexcept>

#define VK_CHECK(expr)                                                                                     \
    do                                                                                                     \
    {                                                                                                      \
        const VkResult vkCheckResult_ = (expr);                                                            \
        if (vkCheckResult_ != VK_SUCCESS)                                                                  \
        {                                                                                                  \
            throw std::runtime_error(fmt::format("{} failed: VkResult {}", #expr, static_cast<int>(vkCheckResult_))); \
        }                                                                                                  \
    } while (0)

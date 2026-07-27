#pragma once

#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>

namespace lab::core {

// Convert a VkResult to its enum name for diagnostics.
const char* vkResultString(VkResult result);

// Throws std::runtime_error if `result` is not VK_SUCCESS. Prefer the VK_CHECK
// macro below so the failing call site (file:line) is captured automatically.
inline void vkCheck(VkResult result, const char* expr, const char* file, int line) {
    if (result != VK_SUCCESS) {
        spdlog::error("Vulkan call failed: {} -> {} ({}:{})",
                      expr,
                      vkResultString(result),
                      file,
                      line);
        throw std::runtime_error(std::string("Vulkan call failed: ") + expr + " -> " +
                                 vkResultString(result));
    }
}

} // namespace lab::core

// Wraps a Vulkan call and aborts (via exception) on any non-success result.
#define VK_CHECK(expr) ::lab::core::vkCheck((expr), #expr, __FILE__, __LINE__)

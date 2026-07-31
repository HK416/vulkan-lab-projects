#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace lab::bench {

// Frame capture for the pixel-equivalence check: every condition must produce
// the same image as the baseline, so each lab needs a way to dump one
// deterministic frame. Kept out of the labs themselves — the capture path must
// be identical across conditions or it is not a comparison.

// Records "copy the just-rendered swapchain image into `dst`" into an already
// begun command buffer. Call it AFTER the lab's onRender, which leaves the image
// in PRESENT_SRC_KHR; the layout is restored before returning so present still
// works. `dst` must be host-visible and at least extent.width*height*4 bytes.
void recordCapture(VkCommandBuffer cmd, VkImage image, VkExtent2D extent, VkBuffer dst);

// Writes tightly packed BGRA8 pixels (what the swapchain format gives us) as an
// RGBA PNG. Throws on a write failure.
void writePngBgra(const std::string& path, uint32_t width, uint32_t height, const void* bgra);

} // namespace lab::bench

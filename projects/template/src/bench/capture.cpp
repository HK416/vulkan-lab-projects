#include "bench/capture.h"

#include <stdexcept>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace lab::bench {
namespace {

// Coarse barrier on purpose. CommandBuffer::transitionImageLayout infers masks
// from the layout, and PRESENT_SRC_KHR infers srcAccess = NONE — which would not
// make the color writes visible to the copy. Capture runs once per run, so
// paying for ALL_COMMANDS/MEMORY_WRITE here is free and provably correct.
void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void recordCapture(VkCommandBuffer cmd, VkImage image, VkExtent2D extent, VkBuffer dst) {
    barrier(cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.bufferRowLength = 0; // tightly packed
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);

    barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

void writePngBgra(const std::string& path, uint32_t width, uint32_t height, const void* bgra) {
    const auto* src = static_cast<const uint8_t*>(bgra);
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i + 0] = src[i + 2]; // B8G8R8A8_UNORM -> RGBA
        rgba[i + 1] = src[i + 1];
        rgba[i + 2] = src[i + 0];
        rgba[i + 3] = src[i + 3];
    }
    if (!stbi_write_png(path.c_str(),
                        static_cast<int>(width),
                        static_cast<int>(height),
                        4,
                        rgba.data(),
                        static_cast<int>(width) * 4)) {
        throw std::runtime_error("writePngBgra: cannot write " + path);
    }
}

} // namespace lab::bench

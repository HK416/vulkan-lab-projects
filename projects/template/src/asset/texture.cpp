#include "asset/texture.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <ktx.h>
#include <vulkan/vulkan.h>

#include "core/vk_check.h"
#include "render/buffer.h"
#include "render/command.h"
#include "render/context.h"
#include "render/sync.h"

namespace lab::asset {
namespace {

// Full-subresource layout barrier (sync2). command.h's transitionImageLayout
// hardcodes layerCount = 1, so it can't cover a 6-face cubemap — do it here with
// REMAINING mip/array ranges.
void imageBarrier(VkCommandBuffer cmd,
                  VkImage image,
                  VkImageLayout oldLayout,
                  VkImageLayout newLayout,
                  VkPipelineStageFlags2 srcStage,
                  VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

render::GpuImage loadKtx(render::Context& context, const std::string& path) {
    ktxTexture2* kTex = nullptr;
    if (ktxTexture2_CreateFromNamedFile(path.c_str(),
                                        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                        &kTex) != KTX_SUCCESS) {
        throw std::runtime_error("loadKtx: cannot load " + path);
    }
    // RAII: free the ktx texture on any exit.
    struct KtxGuard {
        ktxTexture2* t;
        ~KtxGuard() {
            ktxTexture_Destroy(ktxTexture(t));
        }
    } guard{kTex};
    auto* base = ktxTexture(kTex);

    // Pre-compressed BC7 material textures / KTX cubemaps carry a concrete
    // VkFormat. A Basis-Universal file that still needs transcoding reports
    // UNDEFINED — not supported here (textures are pre-transcoded offline).
    VkFormat format = static_cast<VkFormat>(kTex->vkFormat);
    if (format == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error("loadKtx: undefined VkFormat (needs transcoding?): " + path);
    }

    const uint32_t levels = base->numLevels;
    const uint32_t layers = base->numLayers; // array layers (usually 1)
    const uint32_t faces = base->numFaces;   // 6 for a cubemap, else 1
    const bool isCubemap = base->isCubemap;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = isCubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {base->baseWidth, base->baseHeight, 1};
    imageInfo.mipLevels = levels;
    imageInfo.arrayLayers = layers * faces;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    render::GpuImage image =
        render::GpuImage::create(context,
                                 imageInfo,
                                 isCubemap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
                                 VK_IMAGE_ASPECT_COLOR_BIT);

    // One staging buffer holds the whole mip chain exactly as libktx laid it out;
    // each subresource copy reads from its recorded offset.
    const ktx_size_t dataSize = ktxTexture_GetDataSize(base);
    render::GpuBuffer staging =
        render::GpuBuffer::createHostVisible(context, dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped(), ktxTexture_GetData(base), dataSize);

    std::vector<VkBufferImageCopy> regions;
    regions.reserve(static_cast<size_t>(levels) * layers * faces);
    for (uint32_t level = 0; level < levels; ++level) {
        const uint32_t w = std::max(1u, base->baseWidth >> level);
        const uint32_t h = std::max(1u, base->baseHeight >> level);
        for (uint32_t layer = 0; layer < layers; ++layer) {
            for (uint32_t face = 0; face < faces; ++face) {
                ktx_size_t offset = 0;
                ktxTexture_GetImageOffset(base, level, layer, face, &offset);

                VkBufferImageCopy region{};
                region.bufferOffset = offset;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = level;
                region.imageSubresource.baseArrayLayer = layer * faces + face;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = {w, h, 1};
                regions.push_back(region);
            }
        }
    }

    // Immediate blocking upload: transition -> copy all mips -> transition to
    // shader-read, one submit.
    render::CommandPool pool(context);
    render::CommandBuffer cmd = pool.allocate();
    cmd.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    imageBarrier(cmd.getHandle(),
                 image.handle(),
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                 VK_ACCESS_2_NONE,
                 VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);

    vkCmdCopyBufferToImage(cmd.getHandle(),
                           staging.handle(),
                           image.handle(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()),
                           regions.data());

    imageBarrier(cmd.getHandle(),
                 image.handle(),
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COPY_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_READ_BIT);

    cmd.end();

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd.getHandle();
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;

    render::Fence fence(context);
    VK_CHECK(vkQueueSubmit2(context.getGraphicsQueue(), 1, &submit, fence.getHandle()));
    fence.wait();

    return image;
}

Ibl loadIbl(render::Context& context,
            const std::string& prefilteredEnvPath,
            const std::string& irradiancePath,
            const std::string& brdfLutPath) {
    Ibl ibl;
    ibl.prefilteredEnv = loadKtx(context, prefilteredEnvPath);
    ibl.irradiance = loadKtx(context, irradiancePath);
    ibl.brdfLut = loadKtx(context, brdfLutPath);
    return ibl;
}

} // namespace lab::asset

#include "render/image.h"

#include "core/vk_check.h"
#include "render/context.h"

namespace lab::render {

GpuImage GpuImage::create(Context& context,
                          const VkImageCreateInfo& imageInfo,
                          VkImageViewType viewType,
                          VkImageAspectFlags aspect) {
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    GpuImage out;
    out.m_context = &context;
    out.m_format = imageInfo.format;
    out.m_mipLevels = imageInfo.mipLevels;
    VK_CHECK(vmaCreateImage(context.getAllocator(),
                            &imageInfo,
                            &alloc,
                            &out.m_image,
                            &out.m_allocation,
                            nullptr));

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out.m_image;
    viewInfo.viewType = viewType;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.levelCount = imageInfo.mipLevels;
    viewInfo.subresourceRange.layerCount = imageInfo.arrayLayers;
    VK_CHECK(vkCreateImageView(context.getDevice(), &viewInfo, nullptr, &out.m_view));
    return out;
}

GpuImage::GpuImage(GpuImage&& other) noexcept
    : m_context(other.m_context), m_image(other.m_image), m_view(other.m_view),
      m_allocation(other.m_allocation), m_format(other.m_format), m_mipLevels(other.m_mipLevels) {
    other.m_image = VK_NULL_HANDLE;
    other.m_view = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_format = VK_FORMAT_UNDEFINED;
    other.m_mipLevels = 1;
}

GpuImage& GpuImage::operator=(GpuImage&& other) noexcept {
    if (this != &other) {
        destroy();
        m_context = other.m_context;
        m_image = other.m_image;
        m_view = other.m_view;
        m_allocation = other.m_allocation;
        m_format = other.m_format;
        m_mipLevels = other.m_mipLevels;
        other.m_image = VK_NULL_HANDLE;
        other.m_view = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_format = VK_FORMAT_UNDEFINED;
        other.m_mipLevels = 1;
    }
    return *this;
}

GpuImage::~GpuImage() {
    destroy();
}

void GpuImage::destroy() noexcept {
    if (m_image == VK_NULL_HANDLE) {
        return;
    }
    // View first, then image+allocation. vmaDestroyImage frees both the VkImage
    // and its VMA allocation in one call.
    if (m_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_context->getDevice(), m_view, nullptr);
        m_view = VK_NULL_HANDLE;
    }
    vmaDestroyImage(m_context->getAllocator(), m_image, m_allocation);
    m_image = VK_NULL_HANDLE;
    m_allocation = VK_NULL_HANDLE;
}

} // namespace lab::render

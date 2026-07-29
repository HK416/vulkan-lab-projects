#pragma once

#include <cstdint>

#include <vma/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace lab::render {

class Context;

// Move-only VMA image + default view. Format comes from whoever creates it (BC7
// for the pre-compressed material textures). Mip level count is stored; the
// sampler (mip/anisotropy settings — a fixed experiment control) is created and
// bound by the lab, not here.
class GpuImage {
public:
    GpuImage(const GpuImage&) = delete;
    GpuImage& operator=(const GpuImage&) = delete;

    GpuImage() = default;
    GpuImage(GpuImage&& other) noexcept;
    GpuImage& operator=(GpuImage&& other) noexcept;
    ~GpuImage();

    // Creates a device-local (VMA) image plus a view spanning all mip levels and
    // array layers. format/extent/mips/layers/usage/flags all come from
    // imageInfo, so the caller (the asset loader) drives them from the source
    // file's metadata. A free function in the asset layer, not a friend: render
    // must not depend on asset, so creation stays a neutral factory here.
    static GpuImage create(Context& context,
                           const VkImageCreateInfo& imageInfo,
                           VkImageViewType viewType,
                           VkImageAspectFlags aspect);

    VkImage handle() const {
        return m_image;
    }

    VkImageView view() const {
        return m_view;
    }

    VkFormat format() const {
        return m_format;
    }

    uint32_t mipLevels() const {
        return m_mipLevels;
    }

    bool valid() const {
        return m_image != VK_NULL_HANDLE;
    }

private:
    void destroy() noexcept;

    Context* m_context{nullptr};
    VkImage m_image{VK_NULL_HANDLE};
    VkImageView m_view{VK_NULL_HANDLE};
    VmaAllocation m_allocation{VK_NULL_HANDLE};
    VkFormat m_format{VK_FORMAT_UNDEFINED};
    uint32_t m_mipLevels = 1;
};

} // namespace lab::render

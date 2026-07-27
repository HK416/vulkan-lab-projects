#include "render/swapchain.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "core/vk_check.h"
#include "render/context.h"

namespace lab::render {
namespace {

// Clamp the requested size to what the surface allows. When currentExtent is
// the special 0xFFFFFFFF value the surface lets us choose, so we use the window
// size clamped to the min/max the driver reports.
VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& caps, int width, int height) {
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }
    VkExtent2D extent{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height =
        std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return extent;
}

} // namespace

Swapchain::Swapchain(Context& context, Swapchain* oldSwapchain, int width, int height)
    : m_context(&context) {
    try {
        createSwapchain(oldSwapchain, width, height);
        createImageViews();
        createDepthResources();
    } catch (...) {
        // Constructor failed partway: reclaim what was already created, since the
        // destructor will NOT run for a never-fully-constructed object.
        destroy();
        throw;
    }
}

void Swapchain::createSwapchain(Swapchain* oldSwapchain, int width, int height) {
    VkPhysicalDevice physicalDevice = m_context->getPhysicalDevice();
    VkDevice device = m_context->getDevice();
    VkSurfaceKHR surface = m_context->getSurface();

    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps));

    // Verify our fixed color format is actually supported by the surface.
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());

    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    const bool formatSupported = std::any_of(formats.begin(), formats.end(), [&](const auto& f) {
        return f.format == SWAPCHAIN_IMAGE_FORMAT && f.colorSpace == colorSpace;
    });
    if (!formatSupported) {
        throw std::runtime_error("surface does not support the required swapchain format");
    }

    m_swapchainExtent = chooseExtent(caps, width, height);

    // Request one more than the minimum to reduce the chance of stalling on the
    // driver, clamped to the maximum (0 means "no maximum").
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = SWAPCHAIN_IMAGE_FORMAT;
    info.imageColorSpace = colorSpace;
    info.imageExtent = m_swapchainExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // Always supported; vsync.
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain ? oldSwapchain->m_swapchain : VK_NULL_HANDLE;

    // If graphics and present are different families the images must be shared.
    uint32_t graphics = m_context->getGraphicsQueueFamilyIndex();
    uint32_t present = m_context->getPresentQueueFamilyIndex();
    uint32_t families[] = {graphics, present};
    if (graphics != present) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VK_CHECK(vkCreateSwapchainKHR(device, &info, nullptr, &m_swapchain));

    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(device, m_swapchain, &actualCount, nullptr);
    m_swapchainImages.resize(actualCount);
    vkGetSwapchainImagesKHR(device, m_swapchain, &actualCount, m_swapchainImages.data());

    spdlog::info("Swapchain: {}x{}, {} images",
                 m_swapchainExtent.width,
                 m_swapchainExtent.height,
                 actualCount);
}

void Swapchain::createImageViews() {
    VkDevice device = m_context->getDevice();
    const uint32_t actualCount = static_cast<uint32_t>(m_swapchainImages.size());
    m_swapchainImageViews.resize(actualCount);
    for (uint32_t i = 0; i < actualCount; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = SWAPCHAIN_IMAGE_FORMAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &m_swapchainImageViews[i]));
    }
}

void Swapchain::createDepthResources() {
    VkDevice device = m_context->getDevice();

    // One shared depth buffer, recreated with the swapchain.
    VkImageCreateInfo depthInfo{};
    depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthInfo.imageType = VK_IMAGE_TYPE_2D;
    depthInfo.format = DEPTH_IMAGE_FORMAT;
    depthInfo.extent = {m_swapchainExtent.width, m_swapchainExtent.height, 1};
    depthInfo.mipLevels = 1;
    depthInfo.arrayLayers = 1;
    depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo depthAlloc{};
    depthAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    depthAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VK_CHECK(vmaCreateImage(m_context->getAllocator(),
                            &depthInfo,
                            &depthAlloc,
                            &m_depthImage,
                            &m_depthImageAllocation,
                            nullptr));

    VkImageViewCreateInfo depthViewInfo{};
    depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image = m_depthImage;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = DEPTH_IMAGE_FORMAT;
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device, &depthViewInfo, nullptr, &m_depthImageView));
}

Swapchain::~Swapchain() {
    destroy();
}

void Swapchain::destroy() noexcept {
    VkDevice device = m_context->getDevice();

    if (m_depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, m_depthImageView, nullptr);
    }
    if (m_depthImage != VK_NULL_HANDLE) {
        vmaDestroyImage(m_context->getAllocator(), m_depthImage, m_depthImageAllocation);
    }
    for (VkImageView view : m_swapchainImageViews) {
        vkDestroyImageView(device, view, nullptr);
    }
    if (m_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, m_swapchain, nullptr);
    }
}

} // namespace lab::render

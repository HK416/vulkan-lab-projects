#pragma once

#include <vector>

#include <vma/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace lab::render {

class Context;

class Swapchain {
public:
    static constexpr VkFormat SWAPCHAIN_IMAGE_FORMAT = VK_FORMAT_B8G8R8A8_UNORM;
    static constexpr VkFormat DEPTH_IMAGE_FORMAT = VK_FORMAT_D32_SFLOAT;

    Swapchain() = delete;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // uncappedPresent: prefer VK_PRESENT_MODE_IMMEDIATE_KHR (no vsync) when the
    // surface supports it, else fall back to FIFO with a warning. The experiment
    // needs vsync off so GPU frame time is not clamped to the refresh rate.
    Swapchain(Context& context,
              Swapchain* oldSwapchain,
              int width,
              int height,
              bool uncappedPresent = false);

    ~Swapchain();

    VkExtent2D getExtent() const {
        return m_swapchainExtent;
    }

    VkSwapchainKHR getSwapchain() const {
        return m_swapchain;
    }

    const std::vector<VkImage>& getImages() const {
        return m_swapchainImages;
    }

    const std::vector<VkImageView>& getImageViews() const {
        return m_swapchainImageViews;
    }

    VkImage getDepthImage() const {
        return m_depthImage;
    }

    VkImageView getDepthImageView() const {
        return m_depthImageView;
    }

private:
    // Construction split into stages (called in order from the constructor).
    void createSwapchain(Swapchain* oldSwapchain, int width, int height, bool uncappedPresent);
    void createImageViews();
    void createDepthResources();

    // Destroys every owned handle. noexcept and null-safe so it can run from
    // both the destructor and a failed constructor.
    void destroy() noexcept;

    Context* m_context;

    VkExtent2D m_swapchainExtent;
    VkSwapchainKHR m_swapchain{VK_NULL_HANDLE};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;

    VkImage m_depthImage{VK_NULL_HANDLE};
    VkImageView m_depthImageView{VK_NULL_HANDLE};
    VmaAllocation m_depthImageAllocation{VK_NULL_HANDLE};
};

} // namespace lab::render

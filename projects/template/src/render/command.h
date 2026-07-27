#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace lab::render {

class Context;

class CommandBuffer {
public:
    CommandBuffer() = delete;
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    // Movable: the handle is non-owning (the pool frees it), so moving is a
    // trivial handle copy. Needed to store CommandBuffers in a std::vector.
    CommandBuffer(CommandBuffer&&) = default;
    CommandBuffer& operator=(CommandBuffer&&) = default;

    explicit CommandBuffer(VkCommandBuffer cmdBuffer) : m_cmdBuffer(cmdBuffer) {}

    void begin(VkCommandBufferUsageFlags flags = 0);
    void end();
    void transitionImageLayout(VkImage image,
                               VkImageLayout oldLayout,
                               VkImageLayout newLayout,
                               VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               uint32_t levelCount = 1);

    void copyBuffer(VkBuffer src,
                    VkBuffer dst,
                    VkDeviceSize size,
                    VkDeviceSize srcOffset = 0,
                    VkDeviceSize dstOffset = 0);

    VkCommandBuffer getHandle() const {
        return m_cmdBuffer;
    }

private:
    VkCommandBuffer m_cmdBuffer{VK_NULL_HANDLE};
};

// A command pool tied to the graphics queue family, with a small free-list so
// buffers handed out by allocate() can be recycled after reset().
class CommandPool {
public:
    CommandPool() = delete;
    CommandPool(const CommandPool&) = delete;
    CommandPool& operator=(const CommandPool&) = delete;

    explicit CommandPool(Context& context);

    ~CommandPool();

    // Recycles every buffer allocated from this pool (they become available to
    // allocate() again). Does not free them back to the driver.
    void reset();

    // Returns a primary command buffer, reusing a recycled one when possible.
    CommandBuffer allocate();

    // Returns `count` primary command buffers.
    std::vector<CommandBuffer> allocate(uint32_t count);

    VkCommandPool getHandle() const {
        return m_pool;
    }

private:
    Context* m_context;

    VkCommandPool m_pool{VK_NULL_HANDLE};
    std::vector<VkCommandBuffer> m_allocated;
    std::vector<VkCommandBuffer> m_freeList;
};

} // namespace lab::render

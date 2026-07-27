#include "render/command.h"

#include "core/vk_check.h"
#include "render/context.h"

namespace lab::render {
namespace {

// Infers reasonable synchronization2 stage/access masks for a layout, so callers
// of transitionImageLayout only need to name the layouts. Covers the layouts a
// lab commonly transitions between; anything else falls back to a coarse
// all-commands / all-memory barrier.
void masksForLayout(VkImageLayout layout, VkPipelineStageFlags2& stage, VkAccessFlags2& access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
    case VK_IMAGE_LAYOUT_PREINITIALIZED:
        stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        access = VK_ACCESS_2_NONE;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_COPY_BIT;
        access = VK_ACCESS_2_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_COPY_BIT;
        access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        access = VK_ACCESS_2_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_2_NONE;
        break;
    default:
        stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        break;
    }
}

} // namespace

// --- CommandBuffer --------------------------------------------------------

void CommandBuffer::begin(VkCommandBufferUsageFlags flags) {
    VkCommandBufferBeginInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    info.flags = flags;
    VK_CHECK(vkBeginCommandBuffer(m_cmdBuffer, &info));
}

void CommandBuffer::end() {
    VK_CHECK(vkEndCommandBuffer(m_cmdBuffer));
}

void CommandBuffer::transitionImageLayout(VkImage image,
                                          VkImageLayout oldLayout,
                                          VkImageLayout newLayout,
                                          VkImageAspectFlags aspectMask,
                                          uint32_t levelCount) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    masksForLayout(oldLayout, barrier.srcStageMask, barrier.srcAccessMask);
    masksForLayout(newLayout, barrier.dstStageMask, barrier.dstAccessMask);
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.levelCount = levelCount;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(m_cmdBuffer, &dependency);
}

void CommandBuffer::copyBuffer(VkBuffer src,
                               VkBuffer dst,
                               VkDeviceSize size,
                               VkDeviceSize srcOffset,
                               VkDeviceSize dstOffset) {
    VkBufferCopy region{};
    region.srcOffset = srcOffset;
    region.dstOffset = dstOffset;
    region.size = size;
    vkCmdCopyBuffer(m_cmdBuffer, src, dst, 1, &region);
}

// --- CommandPool ----------------------------------------------------------

CommandPool::CommandPool(Context& context) : m_context(&context) {
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // Allow individual buffers to be re-recorded (reset) without freeing them.
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = context.getGraphicsQueueFamilyIndex();
    VK_CHECK(vkCreateCommandPool(context.getDevice(), &info, nullptr, &m_pool));
}

CommandPool::~CommandPool() {
    if (m_pool != VK_NULL_HANDLE) {
        // Destroying the pool frees every buffer allocated from it.
        vkDestroyCommandPool(m_context->getDevice(), m_pool, nullptr);
    }
}

void CommandPool::reset() {
    VK_CHECK(vkResetCommandPool(m_context->getDevice(), m_pool, 0));
    // Everything handed out is now reusable again.
    m_freeList = m_allocated;
}

CommandBuffer CommandPool::allocate() {
    if (!m_freeList.empty()) {
        VkCommandBuffer handle = m_freeList.back();
        m_freeList.pop_back();
        return CommandBuffer(handle);
    }

    VkCommandBufferAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = m_pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;

    VkCommandBuffer handle = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(m_context->getDevice(), &info, &handle));
    m_allocated.push_back(handle);
    return CommandBuffer(handle);
}

std::vector<CommandBuffer> CommandPool::allocate(uint32_t count) {
    std::vector<CommandBuffer> buffers;
    // CommandBuffer is neither copyable nor movable, so reserve up front and
    // emplace by handle — no reallocation and no element move.
    buffers.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        buffers.emplace_back(allocate().getHandle());
    }
    return buffers;
}

} // namespace lab::render

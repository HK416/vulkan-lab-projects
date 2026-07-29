#include "render/buffer.h"

#include <cstring>
#include <utility>

#include "core/vk_check.h"
#include "render/command.h"
#include "render/context.h"
#include "render/sync.h"

namespace lab::render {
namespace {

// Raw result of one vmaCreateBuffer — the create* factories copy these into a
// GpuBuffer's private members (which anon-namespace code cannot touch directly).
struct RawBuffer {
    VkBuffer buffer;
    VmaAllocation allocation;
    void* mapped; // null unless the mapped flag was requested
};

RawBuffer makeBuffer(Context& context,
                     VkDeviceSize size,
                     VkBufferUsageFlags usage,
                     VmaAllocationCreateFlags allocFlags) {
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = allocFlags;

    RawBuffer out{};
    VmaAllocationInfo result{};
    VK_CHECK(vmaCreateBuffer(context.getAllocator(),
                             &bufInfo,
                             &allocInfo,
                             &out.buffer,
                             &out.allocation,
                             &result));
    out.mapped = result.pMappedData; // null when MAPPED_BIT not set
    return out;
}

// Records every queued copy into one command buffer, submits once and blocks
// until the transfer completes. Shared by uploadDeviceLocal and StagingUploader.
void submitCopiesBlocking(Context& context, CommandBuffer& cmd) {
    cmd.end();

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd.getHandle();

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;

    Fence fence(context);
    VK_CHECK(vkQueueSubmit2(context.getGraphicsQueue(), 1, &submit, fence.getHandle()));
    fence.wait();
}

} // namespace

// --- GpuBuffer ------------------------------------------------------------

GpuBuffer
GpuBuffer::createHostVisible(Context& context, VkDeviceSize size, VkBufferUsageFlags usage) {
    RawBuffer raw = makeBuffer(context,
                               size,
                               usage,
                               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                   VMA_ALLOCATION_CREATE_MAPPED_BIT);
    GpuBuffer out;
    out.m_context = &context;
    out.m_buffer = raw.buffer;
    out.m_allocation = raw.allocation;
    out.m_size = size;
    out.m_mapped = raw.mapped;
    return out;
}

GpuBuffer
GpuBuffer::createDeviceLocal(Context& context, VkDeviceSize size, VkBufferUsageFlags usage) {
    // Filled via staging, so it is always a copy destination — fold in the usage
    // bit so callers can't forget it.
    RawBuffer raw = makeBuffer(context, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    GpuBuffer out;
    out.m_context = &context;
    out.m_buffer = raw.buffer;
    out.m_allocation = raw.allocation;
    out.m_size = size;
    out.m_mapped = raw.mapped; // null for device-local
    return out;
}

GpuBuffer::GpuBuffer(GpuBuffer&& other) noexcept
    : m_context(other.m_context), m_buffer(other.m_buffer), m_allocation(other.m_allocation),
      m_size(other.m_size), m_mapped(other.m_mapped) {
    other.m_buffer = VK_NULL_HANDLE;
    other.m_allocation = VK_NULL_HANDLE;
    other.m_size = 0;
    other.m_mapped = nullptr;
}

GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept {
    if (this != &other) {
        destroy();
        m_context = other.m_context;
        m_buffer = other.m_buffer;
        m_allocation = other.m_allocation;
        m_size = other.m_size;
        m_mapped = other.m_mapped;
        other.m_buffer = VK_NULL_HANDLE;
        other.m_allocation = VK_NULL_HANDLE;
        other.m_size = 0;
        other.m_mapped = nullptr;
    }
    return *this;
}

GpuBuffer::~GpuBuffer() {
    destroy();
}

void GpuBuffer::destroy() noexcept {
    if (m_buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_context->getAllocator(), m_buffer, m_allocation);
        m_buffer = VK_NULL_HANDLE;
        m_allocation = VK_NULL_HANDLE;
        m_mapped = nullptr;
    }
}

// --- one-off upload -------------------------------------------------------

void uploadDeviceLocal(Context& context,
                       GpuBuffer& dst,
                       const void* data,
                       VkDeviceSize size,
                       VkDeviceSize dstOffset) {
    // A one-shot uploader flushes on scope exit — one submit, one wait.
    StagingUploader uploader(context);
    uploader.upload(dst, data, size, dstOffset);
}

// --- StagingUploader ------------------------------------------------------

StagingUploader::StagingUploader(Context& context, VkDeviceSize flushBudget)
    : m_context(&context), m_flushBudget(flushBudget) {}

StagingUploader::~StagingUploader() {
    flush();
}

void StagingUploader::upload(GpuBuffer& dst,
                             const void* data,
                             VkDeviceSize size,
                             VkDeviceSize dstOffset) {
    // Flush first if this copy would push retained staging past the budget, so
    // peak staging memory stays bounded. A single copy larger than the budget
    // still goes through on its own.
    if (!m_pending.empty() && m_pendingBytes + size > m_flushBudget) {
        flush();
    }

    GpuBuffer staging =
        GpuBuffer::createHostVisible(*m_context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped(), data, size);

    Pending pending;
    pending.dst = dst.handle();
    pending.region.srcOffset = 0;
    pending.region.dstOffset = dstOffset;
    pending.region.size = size;

    m_staging.push_back(std::move(staging));
    m_pending.push_back(pending);
    m_pendingBytes += size;
}

void StagingUploader::flush() {
    if (m_pending.empty()) {
        return;
    }

    CommandPool pool(*m_context);
    CommandBuffer cmd = pool.allocate();
    cmd.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    for (size_t i = 0; i < m_pending.size(); ++i) {
        const Pending& p = m_pending[i];
        cmd.copyBuffer(m_staging[i].handle(),
                       p.dst,
                       p.region.size,
                       p.region.srcOffset,
                       p.region.dstOffset);
    }
    submitCopiesBlocking(*m_context, cmd);

    // Transfer done — staging buffers are free to release.
    m_staging.clear();
    m_pending.clear();
    m_pendingBytes = 0;
}

} // namespace lab::render

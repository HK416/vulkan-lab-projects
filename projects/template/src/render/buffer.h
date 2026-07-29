#pragma once

#include <vector>

#include <vma/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace lab::render {

class Context;

// Move-only VMA buffer. Two flavours:
//  - host-visible + persistently mapped: CPU writes directly (UBO, per-frame
//    data, staging source).
//  - device-local: GPU-only; fill via uploadDeviceLocal() (staging + copy).
class GpuBuffer {
public:
    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;

    GpuBuffer() = default;
    GpuBuffer(GpuBuffer&& other) noexcept;
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;
    ~GpuBuffer();

    // Host-visible, persistently mapped. mapped() returns the CPU pointer.
    static GpuBuffer
    createHostVisible(Context& context, VkDeviceSize size, VkBufferUsageFlags usage);

    // Device-local (GPU-only). Fill with uploadDeviceLocal().
    static GpuBuffer
    createDeviceLocal(Context& context, VkDeviceSize size, VkBufferUsageFlags usage);

    VkBuffer handle() const {
        return m_buffer;
    }
    VkDeviceSize size() const {
        return m_size;
    }
    void* mapped() const {
        return m_mapped; // null unless host-visible
    }
    bool valid() const {
        return m_buffer != VK_NULL_HANDLE;
    }

private:
    void destroy() noexcept;

    Context* m_context{nullptr};
    VkBuffer m_buffer{VK_NULL_HANDLE};
    VmaAllocation m_allocation{VK_NULL_HANDLE};
    VkDeviceSize m_size{0};
    void* m_mapped{nullptr};
};

// One-off staging upload into a device-local buffer: creates a temporary staging
// buffer, copies `data`, and submits a single blocking transfer. Convenient for
// isolated buffers (one UBO); for bulk load (many meshes) use StagingUploader so
// the copies share one submit instead of one submit + fence wait each.
void uploadDeviceLocal(Context& context,
                       GpuBuffer& dst,
                       const void* data,
                       VkDeviceSize size,
                       VkDeviceSize dstOffset = 0);

// Batches many device-local uploads into as few submits as possible. Each
// upload() queues a copy and retains its staging buffer; queued copies are
// recorded into a single command buffer and submitted once on flush() (or when
// pending staging exceeds `flushBudget`, which bounds peak memory). Turns N
// per-mesh submits into ~total_bytes / flushBudget submits.
class StagingUploader {
public:
    StagingUploader() = delete;
    StagingUploader(const StagingUploader&) = delete;
    StagingUploader& operator=(const StagingUploader&) = delete;

    explicit StagingUploader(Context& context, VkDeviceSize flushBudget = 256ull * 1024 * 1024);
    ~StagingUploader(); // flushes any pending copies

    // Queues a copy into an already-created device-local buffer. May trigger an
    // implicit flush() first if adding this copy would exceed flushBudget.
    void upload(GpuBuffer& dst, const void* data, VkDeviceSize size, VkDeviceSize dstOffset = 0);

    // Records all pending copies into one command buffer, submits once, and waits
    // for completion, then releases the retained staging buffers.
    void flush();

private:
    struct Pending {
        VkBuffer dst = VK_NULL_HANDLE;
        VkBufferCopy region{};
    };

    Context* m_context;
    VkDeviceSize m_flushBudget;
    VkDeviceSize m_pendingBytes = 0;
    std::vector<GpuBuffer> m_staging; // retained until flush(), index-aligned with m_pending
    std::vector<Pending> m_pending;
};

} // namespace lab::render

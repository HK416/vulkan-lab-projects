#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include <vulkan/vulkan.h>

namespace lab::render {

class Context;

// Thin descriptor helpers — enough to remove the repeated boilerplate every lab
// hits, and no more. Deliberately NOT a descriptor engine: no set caching, no
// SPIR-V reflection, no per-set freeing. The binding strategy itself (B0 per
// material vs B1 bindless) is the thing under test, so these stay general enough
// to build both and impose neither.

// RAII VkDescriptorSetLayout. The Builder's per-binding `flags` carry
// descriptor-indexing bits, so the same helper builds a classic per-material
// layout (flags = 0) and a bindless array (PARTIALLY_BOUND | UPDATE_AFTER_BIND
// with a large count).
class DescriptorSetLayout {
public:
    DescriptorSetLayout(const DescriptorSetLayout&) = delete;
    DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;

    DescriptorSetLayout() = default;
    DescriptorSetLayout(DescriptorSetLayout&& other) noexcept;
    DescriptorSetLayout& operator=(DescriptorSetLayout&& other) noexcept;
    ~DescriptorSetLayout();

    VkDescriptorSetLayout handle() const {
        return m_layout;
    }

    class Builder {
    public:
        explicit Builder(Context& context);

        Builder& binding(uint32_t binding,
                         VkDescriptorType type,
                         VkShaderStageFlags stages,
                         uint32_t count = 1,
                         VkDescriptorBindingFlags flags = 0);

        // createFlags takes UPDATE_AFTER_BIND_POOL for a bindless set.
        DescriptorSetLayout build(VkDescriptorSetLayoutCreateFlags createFlags = 0);

    private:
        Context* m_context;
        std::vector<VkDescriptorSetLayoutBinding> m_bindings;
        std::vector<VkDescriptorBindingFlags> m_bindingFlags;
    };

private:
    Context* m_context{nullptr};
    VkDescriptorSetLayout m_layout{VK_NULL_HANDLE};
};

// RAII VkDescriptorPool. Pass UPDATE_AFTER_BIND for a bindless pool. Sets are not
// freed individually — reset the whole pool between conditions.
class DescriptorPool {
public:
    DescriptorPool(const DescriptorPool&) = delete;
    DescriptorPool& operator=(const DescriptorPool&) = delete;

    DescriptorPool() = default;
    DescriptorPool(DescriptorPool&& other) noexcept;
    DescriptorPool& operator=(DescriptorPool&& other) noexcept;
    ~DescriptorPool();

    static DescriptorPool create(Context& context,
                                 uint32_t maxSets,
                                 const std::vector<VkDescriptorPoolSize>& sizes,
                                 VkDescriptorPoolCreateFlags flags = 0);

    // Allocates one set. For a set whose last binding is a variable-count
    // (bindless) array, pass variableCount = the actual element count.
    VkDescriptorSet allocate(VkDescriptorSetLayout layout, uint32_t variableCount = 0);

    void reset(); // recycle every set allocated from this pool

    VkDescriptorPool handle() const {
        return m_pool;
    }

private:
    Context* m_context{nullptr};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
};

// Batches VkWriteDescriptorSet updates and flushes them in one
// vkUpdateDescriptorSets. The set handle is pool-owned; this only writes it.
// Buffer/image infos are copied into internal stable storage so their pointers
// survive until flush().
class DescriptorWriter {
public:
    explicit DescriptorWriter(Context& context);

    DescriptorWriter& writeBuffer(VkDescriptorSet set,
                                  uint32_t binding,
                                  VkDescriptorType type,
                                  const VkDescriptorBufferInfo& info);

    DescriptorWriter& writeImage(VkDescriptorSet set,
                                 uint32_t binding,
                                 VkDescriptorType type,
                                 const VkDescriptorImageInfo& info,
                                 uint32_t arrayElement = 0);

    void flush(); // applies and clears all queued writes

private:
    Context* m_context;
    std::vector<VkWriteDescriptorSet> m_writes;
    // deque, not vector: element pointers must stay valid as more are queued.
    std::deque<VkDescriptorBufferInfo> m_bufferInfos;
    std::deque<VkDescriptorImageInfo> m_imageInfos;
};

} // namespace lab::render

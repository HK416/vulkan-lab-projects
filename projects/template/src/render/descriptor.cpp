#include "render/descriptor.h"

#include "core/vk_check.h"
#include "render/context.h"

namespace lab::render {

// --- DescriptorSetLayout --------------------------------------------------

DescriptorSetLayout::Builder::Builder(Context& context) : m_context(&context) {}

DescriptorSetLayout::Builder&
DescriptorSetLayout::Builder::binding(uint32_t binding,
                                      VkDescriptorType type,
                                      VkShaderStageFlags stages,
                                      uint32_t count,
                                      VkDescriptorBindingFlags flags) {
    VkDescriptorSetLayoutBinding b{};
    b.binding = binding;
    b.descriptorType = type;
    b.descriptorCount = count;
    b.stageFlags = stages;
    m_bindings.push_back(b);
    m_bindingFlags.push_back(flags); // index-aligned with m_bindings
    return *this;
}

DescriptorSetLayout
DescriptorSetLayout::Builder::build(VkDescriptorSetLayoutCreateFlags createFlags) {
    // The per-binding flags (PARTIALLY_BOUND, UPDATE_AFTER_BIND, VARIABLE_COUNT)
    // ride in via pNext. Harmless when every flag is 0 (classic layout).
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.bindingCount = static_cast<uint32_t>(m_bindingFlags.size());
    flagsInfo.pBindingFlags = m_bindingFlags.empty() ? nullptr : m_bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.pNext = &flagsInfo;
    info.flags = createFlags;
    info.bindingCount = static_cast<uint32_t>(m_bindings.size());
    info.pBindings = m_bindings.empty() ? nullptr : m_bindings.data();

    DescriptorSetLayout out;
    out.m_context = m_context;
    VK_CHECK(vkCreateDescriptorSetLayout(m_context->getDevice(), &info, nullptr, &out.m_layout));
    return out;
}

DescriptorSetLayout::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept
    : m_context(other.m_context), m_layout(other.m_layout) {
    other.m_layout = VK_NULL_HANDLE;
}

DescriptorSetLayout& DescriptorSetLayout::operator=(DescriptorSetLayout&& other) noexcept {
    if (this != &other) {
        if (m_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_context->getDevice(), m_layout, nullptr);
        }
        m_context = other.m_context;
        m_layout = other.m_layout;
        other.m_layout = VK_NULL_HANDLE;
    }
    return *this;
}

DescriptorSetLayout::~DescriptorSetLayout() {
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_context->getDevice(), m_layout, nullptr);
    }
}

// --- DescriptorPool -------------------------------------------------------

DescriptorPool DescriptorPool::create(Context& context,
                                      uint32_t maxSets,
                                      const std::vector<VkDescriptorPoolSize>& sizes,
                                      VkDescriptorPoolCreateFlags flags) {
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = flags;
    info.maxSets = maxSets;
    info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    info.pPoolSizes = sizes.empty() ? nullptr : sizes.data();

    DescriptorPool out;
    out.m_context = &context;
    VK_CHECK(vkCreateDescriptorPool(context.getDevice(), &info, nullptr, &out.m_pool));
    return out;
}

VkDescriptorSet DescriptorPool::allocate(VkDescriptorSetLayout layout, uint32_t variableCount) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = m_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;

    // A variable-count (bindless) tail binding needs its actual element count
    // supplied at allocation time.
    VkDescriptorSetVariableDescriptorCountAllocateInfo variableInfo{};
    if (variableCount > 0) {
        variableInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
        variableInfo.descriptorSetCount = 1;
        variableInfo.pDescriptorCounts = &variableCount;
        info.pNext = &variableInfo;
    }

    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(m_context->getDevice(), &info, &set));
    return set;
}

void DescriptorPool::reset() {
    VK_CHECK(vkResetDescriptorPool(m_context->getDevice(), m_pool, 0));
}

DescriptorPool::DescriptorPool(DescriptorPool&& other) noexcept
    : m_context(other.m_context), m_pool(other.m_pool) {
    other.m_pool = VK_NULL_HANDLE;
}

DescriptorPool& DescriptorPool::operator=(DescriptorPool&& other) noexcept {
    if (this != &other) {
        if (m_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_context->getDevice(), m_pool, nullptr);
        }
        m_context = other.m_context;
        m_pool = other.m_pool;
        other.m_pool = VK_NULL_HANDLE;
    }
    return *this;
}

DescriptorPool::~DescriptorPool() {
    if (m_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_context->getDevice(), m_pool, nullptr);
    }
}

// --- DescriptorWriter -----------------------------------------------------

DescriptorWriter::DescriptorWriter(Context& context) : m_context(&context) {}

DescriptorWriter& DescriptorWriter::writeBuffer(VkDescriptorSet set,
                                                uint32_t binding,
                                                VkDescriptorType type,
                                                const VkDescriptorBufferInfo& info) {
    // Copy into stable deque storage so the pointer survives until flush().
    const VkDescriptorBufferInfo& stored = (m_bufferInfos.push_back(info), m_bufferInfos.back());

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &stored;
    m_writes.push_back(write);
    return *this;
}

DescriptorWriter& DescriptorWriter::writeImage(VkDescriptorSet set,
                                               uint32_t binding,
                                               VkDescriptorType type,
                                               const VkDescriptorImageInfo& info,
                                               uint32_t arrayElement) {
    const VkDescriptorImageInfo& stored = (m_imageInfos.push_back(info), m_imageInfos.back());

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = binding;
    write.dstArrayElement = arrayElement;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pImageInfo = &stored;
    m_writes.push_back(write);
    return *this;
}

void DescriptorWriter::flush() {
    if (!m_writes.empty()) {
        vkUpdateDescriptorSets(m_context->getDevice(),
                               static_cast<uint32_t>(m_writes.size()),
                               m_writes.data(),
                               0,
                               nullptr);
    }
    m_writes.clear();
    m_bufferInfos.clear();
    m_imageInfos.clear();
}

} // namespace lab::render

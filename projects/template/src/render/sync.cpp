#include "render/sync.h"

#include "core/vk_check.h"
#include "render/context.h"

namespace lab::render {

// --- TimelineSemaphore ----------------------------------------------------

TimelineSemaphore::TimelineSemaphore(Context& context, uint64_t initialValue)
    : m_context(&context) {
    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = initialValue;

    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &typeInfo;
    VK_CHECK(vkCreateSemaphore(context.getDevice(), &info, nullptr, &m_semaphore));
}

TimelineSemaphore::~TimelineSemaphore() {
    if (m_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_context->getDevice(), m_semaphore, nullptr);
    }
}

VkResult TimelineSemaphore::wait(uint64_t value, uint64_t timeoutNanoseconds) {
    VkSemaphoreWaitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &m_semaphore;
    info.pValues = &value;
    return vkWaitSemaphores(m_context->getDevice(), &info, timeoutNanoseconds);
}

void TimelineSemaphore::signal(uint64_t value) {
    VkSemaphoreSignalInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    info.semaphore = m_semaphore;
    info.value = value;
    VK_CHECK(vkSignalSemaphore(m_context->getDevice(), &info));
}

uint64_t TimelineSemaphore::getValue() const {
    uint64_t value = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(m_context->getDevice(), m_semaphore, &value));
    return value;
}

// --- BinarySemaphore ------------------------------------------------------

BinarySemaphore::BinarySemaphore(Context& context) : m_context(&context) {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_CHECK(vkCreateSemaphore(context.getDevice(), &info, nullptr, &m_semaphore));
}

BinarySemaphore::~BinarySemaphore() {
    if (m_semaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(m_context->getDevice(), m_semaphore, nullptr);
    }
}

// --- Fence ----------------------------------------------------------------

Fence::Fence(Context& context, bool signaled) : m_context(&context) {
    VkFenceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (signaled) {
        info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    }
    VK_CHECK(vkCreateFence(context.getDevice(), &info, nullptr, &m_fence));
}

Fence::~Fence() {
    if (m_fence != VK_NULL_HANDLE) {
        vkDestroyFence(m_context->getDevice(), m_fence, nullptr);
    }
}

void Fence::wait(uint64_t timeoutNanoseconds) {
    vkWaitForFences(m_context->getDevice(), 1, &m_fence, VK_TRUE, timeoutNanoseconds);
}

void Fence::reset() {
    vkResetFences(m_context->getDevice(), 1, &m_fence);
}

} // namespace lab::render

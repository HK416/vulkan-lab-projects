#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace lab::render {

class Context;

class TimelineSemaphore {
public:
    TimelineSemaphore() = delete;
    TimelineSemaphore(const TimelineSemaphore&) = delete;
    TimelineSemaphore& operator=(const TimelineSemaphore&) = delete;

    TimelineSemaphore(Context& context, uint64_t initialValue = 0);
    ~TimelineSemaphore();

    VkResult wait(uint64_t value, uint64_t timeoutNanoseconds = UINT64_MAX);

    void signal(uint64_t value);

    uint64_t getValue() const;

    VkSemaphore getHandle() const {
        return m_semaphore;
    }

private:
    Context* m_context;
    VkSemaphore m_semaphore{VK_NULL_HANDLE};
};

class BinarySemaphore {
public:
    BinarySemaphore() = delete;
    BinarySemaphore(const BinarySemaphore&) = delete;
    BinarySemaphore& operator=(const BinarySemaphore&) = delete;

    explicit BinarySemaphore(Context& context);
    ~BinarySemaphore();

    VkSemaphore getHandle() const {
        return m_semaphore;
    }

private:
    Context* m_context;
    VkSemaphore m_semaphore{VK_NULL_HANDLE};
};

class Fence {
public:
    Fence() = delete;
    Fence(const Fence&) = delete;
    Fence& operator=(const Fence&) = delete;

    Fence(Context& context, bool signaled = false);
    ~Fence();

    void wait(uint64_t timeoutNanoseconds = UINT64_MAX);
    void reset();

    VkFence getHandle() const {
        return m_fence;
    }

private:
    Context* m_context;
    VkFence m_fence{VK_NULL_HANDLE};
};

} // namespace lab::render

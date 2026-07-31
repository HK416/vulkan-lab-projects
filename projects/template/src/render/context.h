#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <vma/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace lab::render {

// Opt-in device features a lab requests. Baseline features every lab needs
// (dynamic rendering, sync2, timeline semaphores) are always enabled and are
// not listed here. A requested feature is only enabled if the device actually
// supports it, so drawIndirectCount silently downgrades on MoltenVK; query
// Context::enabledFeatures() to branch at runtime.
struct DeviceFeatures {
    bool multiDrawIndirect = false;
    // Indirect commands with firstInstance != 0 — how the object index reaches
    // the shader in every indirect condition. Separate from multiDrawIndirect:
    // a device may offer one without the other.
    bool drawIndirectFirstInstance = false;
    bool drawIndirectCount = false;
    bool descriptorIndexing = false;
    bool shaderDrawParameters = false;
    bool bufferDeviceAddress = false;
};

// Owns the core Vulkan objects shared by every subsystem: instance, surface,
// physical/logical device, queues and the VMA allocator.
//
// The Context is deliberately windowing-system agnostic. The caller supplies
// the instance extensions the window system requires and a callback that turns
// the freshly created instance into a surface, so this class never depends on
// SDL (or any other windowing library) directly.
class Context {
public:
    // Given an instance, produce a presentation surface for it.
    using SurfaceFactory = std::function<VkSurfaceKHR(VkInstance)>;

    Context() = delete;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    Context(const std::vector<const char*>& requiredInstanceExtensions,
            const SurfaceFactory& createSurface,
            const DeviceFeatures& requestedFeatures = {});

    ~Context();

    // The subset of requestedFeatures actually enabled on this device. Labs
    // branch on these (e.g. drawIndirectCount is false on MoltenVK).
    const DeviceFeatures& enabledFeatures() const {
        return m_enabledFeatures;
    }

    VkInstance getInstance() const {
        return m_instance;
    }

    VkSurfaceKHR getSurface() const {
        return m_surface;
    }

    VkPhysicalDevice getPhysicalDevice() const {
        return m_physicalDevice;
    }

    VkDevice getDevice() const {
        return m_device;
    }

    VmaAllocator getAllocator() const {
        return m_allocator;
    }

    VkPipelineCache getPipelineCache() const {
        return m_pipelineCache;
    }

    uint32_t getGraphicsQueueFamilyIndex() const {
        return m_graphicsQueueFamilyIndex;
    }

    VkQueue getGraphicsQueue() const {
        return m_graphicsQueue;
    }

    uint32_t getPresentQueueFamilyIndex() const {
        return m_presentQueueFamilyIndex;
    }

    VkQueue getPresentQueue() const {
        return m_presentQueue;
    }

private:
    // Construction split into stages (called in order from the constructor).
    void createInstance(const std::vector<const char*>& requiredInstanceExtensions);
    void selectPhysicalDevice();
    void createLogicalDevice();
    void createAllocatorAndCache();

    // Destroys every owned handle in reverse creation order. noexcept and
    // null-safe so it can run from both the destructor and a failed constructor.
    void destroy() noexcept;

    DeviceFeatures m_requestedFeatures{};
    DeviceFeatures m_enabledFeatures{};

    VkInstance m_instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};

    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDeviceProperties2 m_properties{};
    VkPhysicalDeviceMemoryProperties m_memoryProperties{};

    VmaAllocator m_allocator{VK_NULL_HANDLE};
    VkPipelineCache m_pipelineCache{VK_NULL_HANDLE};

    uint32_t m_graphicsQueueFamilyIndex{UINT32_MAX};
    VkQueue m_graphicsQueue{VK_NULL_HANDLE};

    uint32_t m_presentQueueFamilyIndex{UINT32_MAX};
    VkQueue m_presentQueue{VK_NULL_HANDLE};
};

} // namespace lab::render

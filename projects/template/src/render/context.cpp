#include "render/context.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <spdlog/spdlog.h>

#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>

#include "core/vk_check.h"

using lab::core::vkResultString;

namespace lab::render {
namespace {

#ifdef NDEBUG
constexpr bool ENABLE_VALIDATION = false;
#else
constexpr bool ENABLE_VALIDATION = true;
#endif

// MoltenVK is a portability driver, so on Apple platforms the instance must opt
// in to enumerating portability devices and the device must enable the
// portability subset extension when it advertises it.
#if defined(__APPLE__)
constexpr bool IS_PORTABILITY_PLATFORM = true;
#else
constexpr bool IS_PORTABILITY_PLATFORM = false;
#endif

// Target Vulkan 1.3 — the highest version MoltenVK supports broadly. Labs that
// need 1.3 features can raise this once they confirm driver support.
constexpr uint32_t API_VERSION = VK_API_VERSION_1_3;

constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

bool hasExtension(const std::vector<VkExtensionProperties>& available, const char* name) {
    return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

bool hasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [&](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, name) == 0;
    });
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* /*userData*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        spdlog::error("[vulkan] {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        spdlog::warn("[vulkan] {}", data->pMessage);
    } else {
        spdlog::debug("[vulkan] {}", data->pMessage);
    }
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT makeDebugMessengerInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;
    return info;
}

} // namespace

Context::Context(const std::vector<const char*>& requiredInstanceExtensions,
                 const SurfaceFactory& createSurface,
                 const DeviceFeatures& requestedFeatures)
    : m_requestedFeatures(requestedFeatures) {
    try {
        createInstance(requiredInstanceExtensions);

        // Surface is delegated to the caller, keeping this class windowing-agnostic.
        m_surface = createSurface(m_instance);
        if (m_surface == VK_NULL_HANDLE) {
            throw std::runtime_error("surface factory returned VK_NULL_HANDLE");
        }

        selectPhysicalDevice();
        createLogicalDevice();
        createAllocatorAndCache();
    } catch (...) {
        // Constructor failed partway: reclaim whatever was already created, since
        // the destructor will NOT run for a never-fully-constructed object.
        destroy();
        throw;
    }
}

void Context::createInstance(const std::vector<const char*>& requiredInstanceExtensions) {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "vulkan-lab";
    appInfo.apiVersion = API_VERSION;

    std::vector<const char*> extensions = requiredInstanceExtensions;
    std::vector<const char*> layers;

    uint32_t availableCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
    std::vector<VkExtensionProperties> availableExtensions(availableCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, availableExtensions.data());

    VkInstanceCreateFlags instanceFlags = 0;
    if (IS_PORTABILITY_PLATFORM &&
        hasExtension(availableExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    const bool useValidation = ENABLE_VALIDATION && hasLayer(VALIDATION_LAYER);
    if (useValidation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back(VALIDATION_LAYER);
    }

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.flags = instanceFlags;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();

    // Chain the messenger info so instance creation/destruction is also covered.
    VkDebugUtilsMessengerCreateInfoEXT debugInfo = makeDebugMessengerInfo();
    if (useValidation) {
        instanceInfo.pNext = &debugInfo;
    }

    VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &m_instance));

    // --- Debug messenger --------------------------------------------------
    if (useValidation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create) {
            VK_CHECK(create(m_instance, &debugInfo, nullptr, &m_debugMessenger));
        }
    }
}

void Context::selectPhysicalDevice() {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("no Vulkan physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

    // Pick a device that can present to our surface and supports a swapchain,
    // preferring a discrete GPU.
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t chosenGraphics = UINT32_MAX;
    uint32_t chosenPresent = UINT32_MAX;
    int chosenScore = -1;

    for (VkPhysicalDevice device : devices) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

        uint32_t graphics = UINT32_MAX;
        uint32_t present = UINT32_MAX;
        for (uint32_t i = 0; i < familyCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT && graphics == UINT32_MAX) {
                graphics = i;
            }
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
            if (presentSupport) {
                // Prefer a family that can do both to keep the pipeline simple.
                if (present == UINT32_MAX || i == graphics) {
                    present = i;
                }
            }
        }
        if (graphics == UINT32_MAX || present == UINT32_MAX) {
            continue;
        }

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> deviceExts(extCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, deviceExts.data());
        if (!hasExtension(deviceExts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }

        uint32_t formatCount = 0;
        uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, nullptr);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &presentModeCount, nullptr);
        if (formatCount == 0 || presentModeCount == 0) {
            continue;
        }

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1000 : 100;
        if (score > chosenScore) {
            chosen = device;
            chosenGraphics = graphics;
            chosenPresent = present;
            chosenScore = score;
        }
    }

    if (chosen == VK_NULL_HANDLE) {
        throw std::runtime_error("no suitable Vulkan device (graphics + present + swapchain)");
    }

    m_physicalDevice = chosen;
    m_graphicsQueueFamilyIndex = chosenGraphics;
    m_presentQueueFamilyIndex = chosenPresent;

    m_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &m_properties);
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &m_memoryProperties);
    spdlog::info("Selected GPU: {}", m_properties.properties.deviceName);
}

void Context::createLogicalDevice() {
    std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> supportedDeviceExts(extCount);
    vkEnumerateDeviceExtensionProperties(m_physicalDevice,
                                         nullptr,
                                         &extCount,
                                         supportedDeviceExts.data());
    // Required by the spec whenever the driver advertises it (MoltenVK does).
    if (hasExtension(supportedDeviceExts, "VK_KHR_portability_subset")) {
        deviceExtensions.push_back("VK_KHR_portability_subset");
    }

    const float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    VkDeviceQueueCreateInfo graphicsQueueInfo{};
    graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    graphicsQueueInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
    graphicsQueueInfo.queueCount = 1;
    graphicsQueueInfo.pQueuePriorities = &queuePriority;
    queueInfos.push_back(graphicsQueueInfo);
    if (m_presentQueueFamilyIndex != m_graphicsQueueFamilyIndex) {
        VkDeviceQueueCreateInfo presentQueueInfo = graphicsQueueInfo;
        presentQueueInfo.queueFamilyIndex = m_presentQueueFamilyIndex;
        queueInfos.push_back(presentQueueInfo);
    }

    // Query what the device actually supports so a lab's requested features can
    // be enabled only where available (drawIndirectCount is absent on MoltenVK).
    VkPhysicalDeviceVulkan11Features supported11{};
    supported11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    supported12.pNext = &supported11;
    VkPhysicalDeviceFeatures2 supported2{};
    supported2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    supported2.pNext = &supported12;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &supported2);

    // enabled = requested AND supported; recorded for labs to branch on.
    const DeviceFeatures& req = m_requestedFeatures;
    DeviceFeatures& en = m_enabledFeatures;
    en.multiDrawIndirect = req.multiDrawIndirect && supported2.features.multiDrawIndirect;
    en.drawIndirectCount = req.drawIndirectCount && supported12.drawIndirectCount;
    en.descriptorIndexing = req.descriptorIndexing && supported12.descriptorIndexing;
    en.shaderDrawParameters = req.shaderDrawParameters && supported11.shaderDrawParameters;
    en.bufferDeviceAddress = req.bufferDeviceAddress && supported12.bufferDeviceAddress;

    // Baseline features every lab needs (always enabled). The structs are chained
    // via pNext (so pEnabledFeatures stays null and core features live in
    // features2.features).
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = &features13;
    features12.timelineSemaphore = VK_TRUE;
    features12.drawIndirectCount = en.drawIndirectCount;
    features12.descriptorIndexing = en.descriptorIndexing;
    // Bindless material arrays need runtime-sized, non-uniform-indexed samplers.
    features12.runtimeDescriptorArray = en.descriptorIndexing;
    features12.shaderSampledImageArrayNonUniformIndexing = en.descriptorIndexing;
    features12.bufferDeviceAddress = en.bufferDeviceAddress;

    VkPhysicalDeviceVulkan11Features features11{};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.pNext = &features12;
    features11.shaderDrawParameters = en.shaderDrawParameters;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features11;
    features2.features.multiDrawIndirect = en.multiDrawIndirect;
    features2.features.drawIndirectFirstInstance = en.multiDrawIndirect;
    // Baseline: every lab benchmarks, so enable pipeline-statistics queries when
    // the device offers them (bench::GpuQueries degrades gracefully if not).
    features2.features.pipelineStatisticsQuery = supported2.features.pipelineStatisticsQuery;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &features2;
    deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
    deviceInfo.pQueueCreateInfos = queueInfos.data();
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VK_CHECK(vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device));

    vkGetDeviceQueue(m_device, m_graphicsQueueFamilyIndex, 0, &m_graphicsQueue);
    vkGetDeviceQueue(m_device, m_presentQueueFamilyIndex, 0, &m_presentQueue);
}

void Context::createAllocatorAndCache() {
    // --- VMA allocator ----------------------------------------------------
    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.vulkanApiVersion = API_VERSION;
    allocatorInfo.instance = m_instance;
    allocatorInfo.physicalDevice = m_physicalDevice;
    allocatorInfo.device = m_device;
    VK_CHECK(vmaCreateAllocator(&allocatorInfo, &m_allocator));

    // --- Pipeline cache (empty; labs may seed it from disk later) ---------
    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    VK_CHECK(vkCreatePipelineCache(m_device, &cacheInfo, nullptr, &m_pipelineCache));
}

Context::~Context() {
    destroy();
}

void Context::destroy() noexcept {
    // Destroy in reverse order of creation. Handles are null-safe to destroy.
    if (m_pipelineCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
    }
    if (m_allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(m_allocator);
    }
    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    }
    if (m_debugMessenger != VK_NULL_HANDLE) {
        auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyMessenger) {
            destroyMessenger(m_instance, m_debugMessenger, nullptr);
        }
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }
}

} // namespace lab::render

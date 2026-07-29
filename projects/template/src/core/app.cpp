#include "core/app.h"

#include <stdexcept>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <spdlog/spdlog.h>

#include "core/vk_check.h"
#include "render/command.h"
#include "render/context.h"
#include "render/swapchain.h"
#include "render/sync.h"

namespace lab::core {

App::App(const std::string& title,
         int width,
         int height,
         const render::DeviceFeatures& features,
         bool uncappedPresent)
    : m_width(width), m_height(height), m_features(features), m_uncappedPresent(uncappedPresent) {
    try {
        initWindowAndContext(title);
        initFrameSync();
    } catch (...) {
        // Vulkan members are RAII (unique_ptr) and clean themselves up; only the
        // raw SDL state needs undoing before the exception propagates.
        if (m_window) {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
        }
        SDL_Quit();
        throw;
    }
}

void App::initWindowAndContext(const std::string& title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    m_window = SDL_CreateWindow(title.c_str(),
                                m_width,
                                m_height,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!m_window) {
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    // Instance extensions the windowing system needs, handed to the Context so
    // it never has to know we are using SDL.
    uint32_t extCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extCount);
    std::vector<const char*> requiredExtensions(sdlExtensions, sdlExtensions + extCount);

    // Surface factory: turns the instance the Context creates into an SDL surface.
    auto createSurface = [this](VkInstance instance) -> VkSurfaceKHR {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(m_window, instance, nullptr, &surface)) {
            throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") +
                                     SDL_GetError());
        }
        return surface;
    };

    m_context = std::make_unique<render::Context>(requiredExtensions, createSurface, m_features);

    // Use the actual drawable size in pixels (may differ from the logical window
    // size on high-DPI displays).
    SDL_GetWindowSizeInPixels(m_window, &m_width, &m_height);
    m_swapchain = std::make_unique<render::Swapchain>(
        *m_context, nullptr, m_width, m_height, m_uncappedPresent);
    m_commandPool = std::make_unique<render::CommandPool>(*m_context);
}

void App::initFrameSync() {
    m_frameCommands = m_commandPool->allocate(FRAMES_IN_FLIGHT);
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        m_imageAvailable[i] = std::make_unique<render::BinarySemaphore>(*m_context);
        // Start signaled so the first frame does not wait forever.
        m_inFlight[i] = std::make_unique<render::Fence>(*m_context, /*signaled=*/true);
    }
    createRenderFinishedSemaphores();
}

void App::createRenderFinishedSemaphores() {
    // One render-finished semaphore per swapchain image (count can change on
    // resize). Caller must ensure the GPU is idle before rebuilding these.
    m_renderFinished.clear();
    const size_t imageCount = m_swapchain->getImages().size();
    m_renderFinished.reserve(imageCount);
    for (size_t i = 0; i < imageCount; ++i) {
        m_renderFinished.push_back(std::make_unique<render::BinarySemaphore>(*m_context));
    }
}

App::~App() {
    if (m_context) {
        // Idle the GPU before any resource is destroyed.
        vkDeviceWaitIdle(m_context->getDevice());
    }
    // Explicit teardown order: sync + command pool + swapchain before the
    // context they depend on, then the window and SDL.
    for (auto& s : m_imageAvailable) {
        s.reset();
    }
    for (auto& s : m_renderFinished) {
        s.reset();
    }
    for (auto& f : m_inFlight) {
        f.reset();
    }
    m_commandPool.reset();
    m_swapchain.reset();
    m_context.reset();

    if (m_window) {
        SDL_DestroyWindow(m_window);
    }
    SDL_Quit();
}

void App::run() {
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                recreateSwapchain();
            }
        }
        drawFrame();
    }

    // Drain outstanding GPU work before teardown.
    vkDeviceWaitIdle(m_context->getDevice());
}

void App::drawFrame() {
    VkDevice device = m_context->getDevice();
    render::Fence& fence = *m_inFlight[m_frame];

    // Wait for this frame slot's previous work to finish, then reuse it.
    fence.wait();

    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(device,
                                             m_swapchain->getSwapchain(),
                                             UINT64_MAX,
                                             m_imageAvailable[m_frame]->getHandle(),
                                             VK_NULL_HANDLE,
                                             &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }

    fence.reset();

    render::CommandBuffer& cmd = m_frameCommands[m_frame];
    cmd.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    FrameContext frame{cmd,
                       imageIndex,
                       m_swapchain->getImages()[imageIndex],
                       m_swapchain->getImageViews()[imageIndex],
                       m_swapchain->getDepthImage(),
                       m_swapchain->getDepthImageView(),
                       m_swapchain->getExtent()};
    onRender(frame);

    cmd.end();

    // Submit with synchronization2: wait on image-available, signal render-finished.
    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = m_imageAvailable[m_frame]->getHandle();
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    // Render-finished is indexed by image, not frame slot (see header).
    VkSemaphore renderFinished = m_renderFinished[imageIndex]->getHandle();
    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = renderFinished;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd.getHandle();

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    VK_CHECK(vkQueueSubmit2(m_context->getGraphicsQueue(), 1, &submit, fence.getHandle()));

    // Presentation uses a binary semaphore (swapchain present cannot wait on a
    // timeline semaphore).
    VkSwapchainKHR swapchain = m_swapchain->getSwapchain();
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    VkResult presentResult = vkQueuePresentKHR(m_context->getPresentQueue(), &present);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchain();
    }

    m_frame = (m_frame + 1) % FRAMES_IN_FLIGHT;
}

void App::recreateSwapchain() {
    SDL_GetWindowSizeInPixels(m_window, &m_width, &m_height);
    if (m_width == 0 || m_height == 0) {
        // Window is minimized; nothing to render to yet.
        return;
    }

    vkDeviceWaitIdle(m_context->getDevice());

    // Build the new swapchain from the old one, then release the old.
    auto old = std::move(m_swapchain);
    m_swapchain = std::make_unique<render::Swapchain>(
        *m_context, old.get(), m_width, m_height, m_uncappedPresent);
    old.reset();

    // Image count may have changed; rebuild the per-image semaphores (GPU is
    // already idle from the wait above).
    createRenderFinishedSemaphores();

    onSwapchainRecreated();
}

} // namespace lab::core

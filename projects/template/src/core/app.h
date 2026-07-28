#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct SDL_Window;

namespace lab::render {

class Context;
class CommandBuffer;
class CommandPool;
class Swapchain;
class BinarySemaphore;
class Fence;

} // namespace lab::render

namespace lab::core {

// Everything a lab needs to record one frame. The command buffer is already
// begun (one-time-submit); the lab records into it and returns. Submission,
// presentation and synchronization are handled by App.
struct FrameContext {
    render::CommandBuffer& cmd;
    uint32_t imageIndex;
    VkImage image;
    VkImageView imageView;
    VkImage depth;
    VkImageView depthView;
    VkExtent2D extent;
};

// Base class for every lab. Owns the SDL window, the shared Vulkan subsystems
// and the whole per-frame loop (acquire, submit, present, resize). Labs only
// override onRender to record their commands.
class App {
public:
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    App(const std::string& title, int width, int height);
    virtual ~App();

    // Pumps SDL events and renders until the window is closed.
    void run();

protected:
    static constexpr uint32_t FRAMES_IN_FLIGHT = 2;

    // Record this frame's commands into frame.cmd. Default does nothing.
    virtual void onRender(const FrameContext& frame) {}

    // Called after the swapchain is rebuilt (e.g. resize). Labs override if they
    // cache anything derived from swapchain images/extent.
    virtual void onSwapchainRecreated() {}

    SDL_Window* m_window{nullptr};
    int m_width{0};
    int m_height{0};

    std::unique_ptr<render::Context> m_context;
    std::unique_ptr<render::Swapchain> m_swapchain;
    std::unique_ptr<render::CommandPool> m_commandPool;

private:
    void initWindowAndContext(const std::string& title);
    void initFrameSync();
    void createRenderFinishedSemaphores();
    void drawFrame();
    void recreateSwapchain();

    std::vector<render::CommandBuffer> m_frameCommands;
    // Per frame-in-flight: gates command-buffer/slot reuse.
    std::array<std::unique_ptr<render::BinarySemaphore>, FRAMES_IN_FLIGHT> m_imageAvailable;
    std::array<std::unique_ptr<render::Fence>, FRAMES_IN_FLIGHT> m_inFlight;
    // Per swapchain image, indexed by acquired image index: a present may still
    // be using the semaphore, so it cannot be tied to the frame-in-flight slot.
    std::vector<std::unique_ptr<render::BinarySemaphore>> m_renderFinished;
    uint32_t m_frame{0};
};

} // namespace lab::core

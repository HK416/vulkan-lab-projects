// lab_00 — minimal example: clear the screen to an animated color every frame.
// App handles the window, device, swapchain and per-frame synchronization; the
// lab only records commands in onRender using dynamic rendering (loadOp clear).

#include <cmath>
#include <cstdint>
#include <exception>

#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>

#include "core/app.h"
#include "render/command.h"

using lab::core::App;
using lab::core::FrameContext;

class ClearScreen : public App {
public:
    ClearScreen() : App("lab_00 - clear screen", 1280, 720) {}

protected:
    void onRender(const FrameContext& frame) override {
        // UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL for rendering.
        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        // Animated color, applied via the attachment's clear load op.
        float t = static_cast<float>(m_tick) * 0.02f;
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = frame.imageView;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color.float32[0] = 0.5f + 0.5f * std::sin(t);
        colorAttachment.clearValue.color.float32[1] = 0.5f + 0.5f * std::sin(t + 2.0f);
        colorAttachment.clearValue.color.float32[2] = 0.5f + 0.5f * std::sin(t + 4.0f);
        colorAttachment.clearValue.color.float32[3] = 1.0f;

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = frame.extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colorAttachment;

        // No draw calls: the clear load op does all the work this lab needs.
        vkCmdBeginRendering(frame.cmd.getHandle(), &rendering);
        vkCmdEndRendering(frame.cmd.getHandle());

        // COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC for presentation.
        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        ++m_tick;
    }

private:
    uint64_t m_tick{0};
};

int main() {
    try {
        ClearScreen app;
        app.run();
    } catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    } catch (...) {
        spdlog::critical("fatal: unknown exception");
        return 1;
    }
    return 0;
}

// lab_01 — GPU-driven indirect rendering study.

#include <exception>

#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>

#include "core/app.h"
#include "render/command.h"

using lab::core::App;
using lab::core::FrameContext;

class IndirectLab : public App {
public:
    IndirectLab() : App("lab_01 - indirect rendering", 1280, 720) {}

protected:
    void onRender(const FrameContext& frame) override {
        VkCommandBuffer handle = frame.cmd.getHandle();

        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = frame.imageView;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color.float32[0] = 0.10f;
        colorAttachment.clearValue.color.float32[1] = 0.10f;
        colorAttachment.clearValue.color.float32[2] = 0.12f;
        colorAttachment.clearValue.color.float32[3] = 1.0f;

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = frame.extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(handle, &rendering);
        vkCmdEndRendering(handle);

        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
};

int main() {
    try {
        IndirectLab app;
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

// lab_00 — minimal example: a Y-spinning cube over a sky-blue background.
// App handles the window, device, swapchain and per-frame synchronization; the
// lab uploads the cube's geometry, builds a pipeline with embedded SPIR-V, and
// records the draw in onRender using dynamic rendering.
//
// Resource management goes through the template's render/ RAII wrappers
// (GpuBuffer, Shader, PipelineLayout, GraphicsPipeline) rather than raw Vulkan,
// so there is no manual cleanup: members release themselves in ~CubeSpin.

#include <array>
#include <cstdint>
#include <exception>
#include <vector>

// Vulkan clip space: depth is [0, 1] (not GL's [-1, 1]). GLM_FORCE_DEPTH_ZERO_TO_ONE
// is set workspace-wide (project_deps), so glm::perspective is already Vulkan-correct.
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>

#include "core/app.h"
#include "render/buffer.h"
#include "render/command.h"
#include "render/context.h"
#include "render/pipeline.h"
#include "render/shader.h"
#include "render/swapchain.h"

using lab::core::App;
using lab::core::FrameContext;
using lab::render::GpuBuffer;
using lab::render::GraphicsPipeline;
using lab::render::PipelineBuilder;
using lab::render::PipelineLayout;
using lab::render::Shader;
using lab::render::StagingUploader;

const std::array<float, 24> VERTICES = {
    -0.5f, -0.5f, 0.5f,  // 0: Front-Bottom-Left
    0.5f,  -0.5f, 0.5f,  // 1: Front-Bottom-Right
    0.5f,  0.5f,  0.5f,  // 2: Front-Top-Right
    -0.5f, 0.5f,  0.5f,  // 3: Front-Top-Left
    -0.5f, -0.5f, -0.5f, // 4: Back-Bottom-Left
    0.5f,  -0.5f, -0.5f, // 5: Back-Bottom-Right
    0.5f,  0.5f,  -0.5f, // 6: Back-Top-Right
    -0.5f, 0.5f,  -0.5f  // 7: Back-Top-Left
};

const std::array<uint32_t, 36> INDICES = {
    0, 1, 2, 2, 3, 0, // Front  (0,1,2,3)
    1, 5, 6, 6, 2, 1, // Right  (1,5,6,2)
    5, 4, 7, 7, 6, 5, // Back   (5,4,7,6)
    4, 0, 3, 3, 7, 4, // Left   (4,0,3,7)
    3, 2, 6, 6, 7, 3, // Top    (3,2,6,7)
    4, 5, 1, 1, 0, 4  // Bottom (4,5,1,0)
};

struct PushConstants {
    glm::mat4 projView{1.0f};
    glm::mat4 model{1.0f};
};

// SPIR-V generated at build time by glslc -mfmt=c (see CMakeLists), baked in.
const uint32_t CUBE_VERT[] =
#include "cube.vert.h"
    ;
const uint32_t CUBE_FRAG[] =
#include "cube.frag.h"
    ;

class CubeSpin : public App {
public:
    CubeSpin() : App("lab_00 - cube", 1280, 720) {
        createBuffers();
        createPipeline();
    }

    ~CubeSpin() override {
        // Members (buffers, pipeline, layout) release on destruction below, while
        // the base App's Context is still alive — so drain the GPU first.
        vkDeviceWaitIdle(m_context->getDevice());
    }

protected:
    void onRender(const FrameContext& frame) override {
        VkCommandBuffer handle = frame.cmd.getHandle();

        // Attachments must be in their optimal layouts before rendering. Depth is
        // UNDEFINED each frame (contents not preserved), so a plain discard-then-
        // clear transition is fine.
        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        frame.cmd.transitionImageLayout(frame.depth,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_ASPECT_DEPTH_BIT);

        // Sky-blue background via the color clear load op.
        VkRenderingAttachmentInfo colorAttachment{};
        colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colorAttachment.imageView = frame.imageView;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color.float32[0] = 0.53f;
        colorAttachment.clearValue.color.float32[1] = 0.81f;
        colorAttachment.clearValue.color.float32[2] = 0.92f;
        colorAttachment.clearValue.color.float32[3] = 1.0f;

        VkRenderingAttachmentInfo depthAttachment{};
        depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView = frame.depthView;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.clearValue.depthStencil.depth = 1.0f;

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = frame.extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colorAttachment;
        rendering.pDepthAttachment = &depthAttachment;

        vkCmdBeginRendering(handle, &rendering);

        // Viewport/scissor are dynamic, so they are set here rather than baked
        // into the pipeline (lets the same pipeline survive swapchain resizes).
        VkViewport viewport{};
        viewport.width = static_cast<float>(frame.extent.width);
        viewport.height = static_cast<float>(frame.extent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(handle, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, frame.extent};
        vkCmdSetScissor(handle, 0, 1, &scissor);

        m_pipeline.bind(handle);
        VkDeviceSize offset = 0;
        VkBuffer vertexBuffer = m_vertexBuffer.handle();
        vkCmdBindVertexBuffers(handle, 0, 1, &vertexBuffer, &offset);
        vkCmdBindIndexBuffer(handle, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);

        // Camera aimed at the cube's top edge; the cube spins about its Y axis.
        float t = static_cast<float>(m_tick) * 0.02f;
        float aspect =
            static_cast<float>(frame.extent.width) / static_cast<float>(frame.extent.height);

        PushConstants pc;
        glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
        proj[1][1] *= -1.0f; // flip Y: Vulkan's clip space points down vs OpenGL.
        glm::mat4 view = glm::lookAt(glm::vec3(2.5f, 2.5f, 2.5f),  // eye
                                     glm::vec3(0.0f, 0.5f, 0.5f),  // top front edge
                                     glm::vec3(0.0f, 1.0f, 0.0f)); // up
        pc.projView = proj * view;
        pc.model = glm::rotate(glm::mat4(1.0f), t, glm::vec3(0.0f, 1.0f, 0.0f));
        vkCmdPushConstants(handle,
                           m_pipeline.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT,
                           0,
                           sizeof(PushConstants),
                           &pc);

        vkCmdDrawIndexed(handle, static_cast<uint32_t>(INDICES.size()), 1, 0, 0, 0);

        vkCmdEndRendering(handle);

        // COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC for presentation.
        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        ++m_tick;
    }

private:
    void createBuffers() {
        m_vertexBuffer = GpuBuffer::createDeviceLocal(*m_context,
                                                      sizeof(VERTICES),
                                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        m_indexBuffer = GpuBuffer::createDeviceLocal(*m_context,
                                                     sizeof(INDICES),
                                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        // Both copies ride one submit; flush() blocks until the upload completes.
        StagingUploader uploader(*m_context);
        uploader.upload(m_vertexBuffer, VERTICES.data(), sizeof(VERTICES));
        uploader.upload(m_indexBuffer, INDICES.data(), sizeof(INDICES));
        uploader.flush();
    }

    void createPipeline() {
        // Push constants only; no descriptor sets.
        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcRange.size = sizeof(PushConstants);
        m_pipelineLayout = PipelineLayout::create(*m_context, {}, {pcRange});

        Shader vert = Shader::fromSpirv(*m_context, CUBE_VERT, sizeof(CUBE_VERT));
        Shader frag = Shader::fromSpirv(*m_context, CUBE_FRAG, sizeof(CUBE_FRAG));

        // Position-only vertex stream (vec3), not the asset layer's full Vertex.
        std::vector<VkVertexInputBindingDescription> bindings = {
            {0, sizeof(float) * 3, VK_VERTEX_INPUT_RATE_VERTEX}};
        std::vector<VkVertexInputAttributeDescription> attributes = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}};

        // PipelineBuilder bakes the fixed render state (depth LESS test+write, back
        // cull / CCW, no blend, 1 sample, dynamic viewport/scissor, swapchain
        // formats) — the same state the old inline setup spelled out by hand.
        m_pipeline = PipelineBuilder(*m_context)
                         .shaders(vert.handle(), frag.handle())
                         .layout(m_pipelineLayout)
                         .vertexInput(bindings, attributes)
                         .build();
    }

    GpuBuffer m_vertexBuffer;
    GpuBuffer m_indexBuffer;
    PipelineLayout m_pipelineLayout;
    GraphicsPipeline m_pipeline;

    uint64_t m_tick{0};
};

int main() {
    try {
        CubeSpin app;
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

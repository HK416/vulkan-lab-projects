// lab_00 — minimal example: a Y-spinning cube over a sky-blue background.
// App handles the window, device, swapchain and per-frame synchronization; the
// lab uploads the cube's geometry, builds a pipeline with embedded SPIR-V, and
// records the draw in onRender using dynamic rendering.

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>

// Vulkan clip space: depth is [0, 1] (not GL's [-1, 1]). Must be set before any
// glm header so glm::perspective produces a Vulkan-correct projection.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>
#include <vma/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include "core/app.h"
#include "core/vk_check.h"
#include "render/command.h"
#include "render/context.h"
#include "render/swapchain.h"

using lab::core::App;
using lab::core::FrameContext;
using lab::render::CommandBuffer;
using lab::render::Context;
using lab::render::Swapchain;

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
    0, 1, 2, // Front Face (using vertices 0, 1, 2, 3)
    2, 3, 0, // Front Face (using vertices 0, 1, 2, 3)

    1, 5, 6, // Right Face (using vertices 1, 5, 6, 2)
    6, 2, 1, // Right Face (using vertices 1, 5, 6, 2)

    5, 4, 7, // Back Face (using vertices 5, 4, 7, 6)
    7, 6, 5, // Back Face (using vertices 5, 4, 7, 6)

    4, 0, 3, // Left Face (using vertices 4, 0, 3, 7)
    3, 7, 4, // Left Face (using vertices 4, 0, 3, 7)

    3, 2, 6, // Top Face (using vertices 3, 2, 6, 7)
    6, 7, 3, // Top Face (using vertices 3, 2, 6, 7)

    4, 5, 1, // Bottom Face (using vertices 4, 5, 1, 0)
    1, 0, 4  // Bottom Face (using vertices 4, 5, 1, 0)
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

struct StagingGuard {
    Context* context{nullptr};
    VkBuffer buffer{VK_NULL_HANDLE};
    VmaAllocation allocation{VK_NULL_HANDLE};

    StagingGuard(Context& context) : context(&context) {}
    ~StagingGuard() {
        if (context != nullptr && buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(context->getAllocator(), buffer, allocation);
        }
    }
};

struct ShaderModule {
    Context* context{nullptr};
    VkShaderModule module{VK_NULL_HANDLE};

    ShaderModule(Context& context) : context(&context) {}
    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;
    ShaderModule(ShaderModule&& o) noexcept : context(o.context), module(o.module) {
        o.module = VK_NULL_HANDLE;
    }
    ~ShaderModule() {
        if (context != nullptr && module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(context->getDevice(), module, nullptr);
        }
    }
};

class CubeSpin : public App {
public:
    CubeSpin() : App("lab_00 - cube", 1280, 720) {
        try {
            createBuffer();
            createPipelineLayout();
            createPipeline();
        } catch (...) {
            vkDeviceWaitIdle(m_context->getDevice());
            cleanup();
            throw;
        }
    }

    ~CubeSpin() override {
        vkDeviceWaitIdle(m_context->getDevice());
        cleanup();
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

        vkCmdBindPipeline(handle, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(handle, 0, 1, &m_vertexBuffer, &offset);
        vkCmdBindIndexBuffer(handle, m_indexBuffer, 0, VK_INDEX_TYPE_UINT32);

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
                           m_pipelineLayout,
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
    void createBuffer() {
        // create vertex staging buffer
        StagingGuard vertexStaging{*m_context};
        {
            VkBufferCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            createInfo.size = static_cast<VkDeviceSize>(sizeof(VERTICES));
            createInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo info{};
            VK_CHECK(vmaCreateBuffer(m_context->getAllocator(),
                                     &createInfo,
                                     &allocInfo,
                                     &vertexStaging.buffer,
                                     &vertexStaging.allocation,
                                     &info));
            memcpy(info.pMappedData, VERTICES.data(), sizeof(VERTICES));
        }

        // create vertex buffer
        {
            VkBufferCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            createInfo.size = static_cast<VkDeviceSize>(sizeof(VERTICES));
            createInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            VK_CHECK(vmaCreateBuffer(m_context->getAllocator(),
                                     &createInfo,
                                     &allocInfo,
                                     &m_vertexBuffer,
                                     &m_vertexAllocation,
                                     nullptr));
        }

        // create index staging buffer
        StagingGuard indexStaging{*m_context};
        {
            VkBufferCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            createInfo.size = static_cast<VkDeviceSize>(sizeof(INDICES));
            createInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo info{};
            VK_CHECK(vmaCreateBuffer(m_context->getAllocator(),
                                     &createInfo,
                                     &allocInfo,
                                     &indexStaging.buffer,
                                     &indexStaging.allocation,
                                     &info));
            memcpy(info.pMappedData, INDICES.data(), sizeof(INDICES));
        }

        // create index buffer
        {
            VkBufferCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            createInfo.size = static_cast<VkDeviceSize>(sizeof(INDICES));
            createInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            VK_CHECK(vmaCreateBuffer(m_context->getAllocator(),
                                     &createInfo,
                                     &allocInfo,
                                     &m_indexBuffer,
                                     &m_indexAllocation,
                                     nullptr));
        }

        // copy staging to buffer
        CommandBuffer cmd = m_commandPool->allocate();
        cmd.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        cmd.copyBuffer(vertexStaging.buffer, m_vertexBuffer, sizeof(VERTICES));
        cmd.copyBuffer(indexStaging.buffer, m_indexBuffer, sizeof(INDICES));
        cmd.end();

        // command submit
        VkCommandBufferSubmitInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = cmd.getHandle();
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        VK_CHECK(vkQueueSubmit2(m_context->getGraphicsQueue(), 1, &submit, VK_NULL_HANDLE));
        vkDeviceWaitIdle(m_context->getDevice());
    }

    void createPipelineLayout() {
        VkPushConstantRange pcRange{};
        pcRange.size = sizeof(PushConstants);
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkPipelineLayoutCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        createInfo.pushConstantRangeCount = 1;
        createInfo.pPushConstantRanges = &pcRange;

        VK_CHECK(vkCreatePipelineLayout(m_context->getDevice(),
                                        &createInfo,
                                        nullptr,
                                        &m_pipelineLayout));
    }

    ShaderModule createShaderModule(const uint32_t* code, size_t byteSize) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = byteSize;
        createInfo.pCode = code;

        ShaderModule module(*m_context);
        VK_CHECK(
            vkCreateShaderModule(m_context->getDevice(), &createInfo, nullptr, &module.module));
        return module;
    }

    void createPipeline() {
        ShaderModule vertModule = createShaderModule(CUBE_VERT, sizeof(CUBE_VERT));
        ShaderModule fragModule = createShaderModule(CUBE_FRAG, sizeof(CUBE_FRAG));

        std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};
        shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shaderStages[0].module = vertModule.module;
        shaderStages[0].pName = "main";

        shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shaderStages[1].module = fragModule.module;
        shaderStages[1].pName = "main";

        VkVertexInputAttributeDescription attribute{};
        attribute.format = VK_FORMAT_R32G32B32_SFLOAT;

        VkVertexInputBindingDescription binding{};
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        binding.stride = sizeof(float) * 3;

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attribute;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.sampleShadingEnable = VK_FALSE;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.logicOpEnable = VK_FALSE;
        colorBlend.logicOp = VK_LOGIC_OP_COPY;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &colorBlendAttachment;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachmentFormats = &Swapchain::SWAPCHAIN_IMAGE_FORMAT;
        renderingInfo.depthAttachmentFormat = Swapchain::DEPTH_IMAGE_FORMAT;

        VkGraphicsPipelineCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        createInfo.pNext = &renderingInfo;
        createInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
        createInfo.pStages = shaderStages.data();
        createInfo.pViewportState = &viewportState;
        createInfo.pVertexInputState = &vertexInput;
        createInfo.pInputAssemblyState = &inputAssembly;
        createInfo.pDepthStencilState = &depthStencil;
        createInfo.pRasterizationState = &rasterizer;
        createInfo.pMultisampleState = &multisample;
        createInfo.pColorBlendState = &colorBlend;
        createInfo.pDynamicState = &dynamicState;
        createInfo.layout = m_pipelineLayout;

        VK_CHECK(vkCreateGraphicsPipelines(m_context->getDevice(),
                                           m_context->getPipelineCache(),
                                           1,
                                           &createInfo,
                                           nullptr,
                                           &m_pipeline));
    }

    void cleanup() {
        if (m_vertexBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_context->getAllocator(), m_vertexBuffer, m_vertexAllocation);
        }

        if (m_indexBuffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(m_context->getAllocator(), m_indexBuffer, m_indexAllocation);
        }

        if (m_pipeline) {
            vkDestroyPipeline(m_context->getDevice(), m_pipeline, nullptr);
        }

        if (m_pipelineLayout) {
            vkDestroyPipelineLayout(m_context->getDevice(), m_pipelineLayout, nullptr);
        }
    }

    VkBuffer m_vertexBuffer{VK_NULL_HANDLE};
    VmaAllocation m_vertexAllocation{VK_NULL_HANDLE};

    VkBuffer m_indexBuffer{VK_NULL_HANDLE};
    VmaAllocation m_indexAllocation{VK_NULL_HANDLE};

    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};

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

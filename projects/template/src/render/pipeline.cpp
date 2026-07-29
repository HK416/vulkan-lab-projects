#include "render/pipeline.h"

#include <array>
#include <stdexcept>

#include "core/vk_check.h"
#include "render/context.h"
#include "render/swapchain.h"

namespace lab::render {

// --- PipelineLayout -------------------------------------------------------

PipelineLayout PipelineLayout::create(Context& context,
                                      const std::vector<VkDescriptorSetLayout>& sets,
                                      const std::vector<VkPushConstantRange>& pushConstants) {
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = static_cast<uint32_t>(sets.size());
    info.pSetLayouts = sets.empty() ? nullptr : sets.data();
    info.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
    info.pPushConstantRanges = pushConstants.empty() ? nullptr : pushConstants.data();

    PipelineLayout out;
    out.m_context = &context;
    VK_CHECK(vkCreatePipelineLayout(context.getDevice(), &info, nullptr, &out.m_layout));
    return out;
}

PipelineLayout::PipelineLayout(PipelineLayout&& other) noexcept
    : m_context(other.m_context), m_layout(other.m_layout) {
    other.m_layout = VK_NULL_HANDLE;
}

PipelineLayout& PipelineLayout::operator=(PipelineLayout&& other) noexcept {
    if (this != &other) {
        if (m_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_context->getDevice(), m_layout, nullptr);
        }
        m_context = other.m_context;
        m_layout = other.m_layout;
        other.m_layout = VK_NULL_HANDLE;
    }
    return *this;
}

PipelineLayout::~PipelineLayout() {
    if (m_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_context->getDevice(), m_layout, nullptr);
    }
}

// --- GraphicsPipeline -----------------------------------------------------

void GraphicsPipeline::bind(VkCommandBuffer cmd) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
}

GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
    : m_context(other.m_context), m_pipeline(other.m_pipeline), m_layout(other.m_layout) {
    other.m_pipeline = VK_NULL_HANDLE;
    other.m_layout = VK_NULL_HANDLE;
}

GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept {
    if (this != &other) {
        destroy();
        m_context = other.m_context;
        m_pipeline = other.m_pipeline;
        m_layout = other.m_layout;
        other.m_pipeline = VK_NULL_HANDLE;
        other.m_layout = VK_NULL_HANDLE;
    }
    return *this;
}

GraphicsPipeline::~GraphicsPipeline() {
    destroy();
}

void GraphicsPipeline::destroy() noexcept {
    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_context->getDevice(), m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
        m_layout = VK_NULL_HANDLE; // borrowed; not destroyed
    }
}

// --- PipelineBuilder ------------------------------------------------------

PipelineBuilder::PipelineBuilder(Context& context) : m_context(&context) {}

PipelineBuilder& PipelineBuilder::shaders(VkShaderModule vert, VkShaderModule frag) {
    m_vert = vert;
    m_frag = frag;
    return *this;
}

PipelineBuilder& PipelineBuilder::layout(const PipelineLayout& layout) {
    m_layout = layout.handle();
    return *this;
}

PipelineBuilder&
PipelineBuilder::vertexInput(const std::vector<VkVertexInputBindingDescription>& bindings,
                             const std::vector<VkVertexInputAttributeDescription>& attributes) {
    m_vertexBindings = bindings;
    m_vertexAttributes = attributes;
    return *this;
}

PipelineBuilder& PipelineBuilder::depthTest(bool testAndWrite) {
    m_depthTest = testAndWrite;
    return *this;
}

GraphicsPipeline PipelineBuilder::build() {
    if (m_vert == VK_NULL_HANDLE || m_frag == VK_NULL_HANDLE) {
        throw std::runtime_error("PipelineBuilder::build: shaders() not provided");
    }
    if (m_layout == VK_NULL_HANDLE) {
        throw std::runtime_error("PipelineBuilder::build: layout() not provided");
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(m_vertexBindings.size());
    vertexInput.pVertexBindingDescriptions =
        m_vertexBindings.empty() ? nullptr : m_vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(m_vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions =
        m_vertexAttributes.empty() ? nullptr : m_vertexAttributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport + scissor are dynamic; counts are fixed at 1, the values come from
    // vkCmdSetViewport/Scissor at record time.
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = m_depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = m_depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                                VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();

    // Dynamic rendering: no VkRenderPass. The attachment formats are baked to the
    // swapchain's, so every condition targets identical attachments.
    VkFormat colorFormat = Swapchain::SWAPCHAIN_IMAGE_FORMAT;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
    rendering.depthAttachmentFormat = Swapchain::DEPTH_IMAGE_FORMAT;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = m_layout;

    GraphicsPipeline out;
    out.m_context = m_context;
    out.m_layout = m_layout;
    VK_CHECK(vkCreateGraphicsPipelines(m_context->getDevice(),
                                       m_context->getPipelineCache(),
                                       1,
                                       &info,
                                       nullptr,
                                       &out.m_pipeline));
    return out;
}

} // namespace lab::render

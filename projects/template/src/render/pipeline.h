#pragma once

#include <vector>

#include <vulkan/vulkan.h>

namespace lab::render {

class Context;

// RAII VkPipelineLayout: the shader interface (descriptor set layouts + push
// constant ranges) as one owning object. One layout is commonly shared by
// several pipelines, so lifetime lives here rather than inside GraphicsPipeline.
// Must outlive every pipeline built against it.
class PipelineLayout {
public:
    PipelineLayout(const PipelineLayout&) = delete;
    PipelineLayout& operator=(const PipelineLayout&) = delete;

    PipelineLayout() = default;
    PipelineLayout(PipelineLayout&& other) noexcept;
    PipelineLayout& operator=(PipelineLayout&& other) noexcept;
    ~PipelineLayout();

    static PipelineLayout create(Context& context,
                                 const std::vector<VkDescriptorSetLayout>& sets,
                                 const std::vector<VkPushConstantRange>& pushConstants = {});

    VkPipelineLayout handle() const {
        return m_layout;
    }

    bool valid() const {
        return m_layout != VK_NULL_HANDLE;
    }

private:
    Context* m_context{nullptr};
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
};

// RAII holder for a graphics pipeline. Neutral: what a condition legitimately
// varies (shaders, layout, vertex input) is supplied by the lab; the fixed
// experiment render state is baked by PipelineBuilder so no condition can drift
// from the controls. The layout is borrowed (owned by a PipelineLayout), so this
// only owns the VkPipeline.
class GraphicsPipeline {
public:
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    GraphicsPipeline() = default;
    GraphicsPipeline(GraphicsPipeline&& other) noexcept;
    GraphicsPipeline& operator=(GraphicsPipeline&& other) noexcept;
    ~GraphicsPipeline();

    void bind(VkCommandBuffer cmd) const; // vkCmdBindPipeline (graphics bind point)

    VkPipeline handle() const {
        return m_pipeline;
    }

    VkPipelineLayout layout() const {
        return m_layout; // borrowed, for vkCmdBindDescriptorSets / push constants
    }

    bool valid() const {
        return m_pipeline != VK_NULL_HANDLE;
    }

private:
    friend class PipelineBuilder;
    void destroy() noexcept;

    Context* m_context{nullptr};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_layout{VK_NULL_HANDLE}; // non-owning
};

// Builds a graphics pipeline with the experiment's fixed, controlled render state
// already set, so every condition is identical on the fixed axes by construction.
//
// Baked defaults (the "must fix" list):
//   - dynamic rendering: color = Swapchain::SWAPCHAIN_IMAGE_FORMAT,
//     depth = Swapchain::DEPTH_IMAGE_FORMAT
//   - depth test + write ON, compare op LESS
//   - no color blending
//   - no MSAA (1 sample)
//   - back-face cull, counter-clockwise front face
//   - dynamic viewport + scissor
//
// The lab must supply shaders(), layout(), and vertexInput() (the last via an
// asset-layer helper, since render must not depend on asset). depthTest() is the
// one optional knob — flipping it changes a control, so it is explicit.
class PipelineBuilder {
public:
    explicit PipelineBuilder(Context& context);

    PipelineBuilder& shaders(VkShaderModule vert, VkShaderModule frag);
    PipelineBuilder& layout(const PipelineLayout& layout); // borrowed, must outlive the pipeline
    PipelineBuilder& vertexInput(const std::vector<VkVertexInputBindingDescription>& bindings,
                                 const std::vector<VkVertexInputAttributeDescription>& attributes);

    PipelineBuilder& depthTest(bool testAndWrite); // default true

    // Creates the pipeline against the supplied layout. Throws if shaders/layout/
    // vertexInput were not provided.
    GraphicsPipeline build();

private:
    Context* m_context;
    VkShaderModule m_vert{VK_NULL_HANDLE};
    VkShaderModule m_frag{VK_NULL_HANDLE};
    VkPipelineLayout m_layout{VK_NULL_HANDLE}; // borrowed from a PipelineLayout
    std::vector<VkVertexInputBindingDescription> m_vertexBindings;
    std::vector<VkVertexInputAttributeDescription> m_vertexAttributes;
    bool m_depthTest = true;
};

} // namespace lab::render

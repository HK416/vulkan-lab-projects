// lab_01 — GPU-driven indirect rendering study. Baseline A0 × B0.
//
//   A0 = one VkBuffer pair (vertex+index) per mesh, plain vkCmdDrawIndexed loop.
//   B0 = one descriptor set per material, vkCmdBindDescriptorSets on change.
//
// The harness lives in template (lab::asset / scene / bench / render). This file
// only WIRES it into the A0 × B0 strategy. Design + data flow: ../README.md and
// ../indirect-rendering-experiment.md.
//

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>

#include "asset/loader.h"
#include "asset/texture.h"
#include "bench/instrument.h"
#include "bench/reporter.h"
#include "core/app.h"
#include "core/vk_check.h"
#include "render/buffer.h"
#include "render/command.h"
#include "render/context.h"
#include "render/descriptor.h"
#include "render/pipeline.h"
#include "render/shader.h"
#include "scene/camera.h"
#include "scene/scene.h"

namespace asset = lab::asset;
namespace render = lab::render;
namespace scene = lab::scene;
namespace bench = lab::bench;

using lab::core::App;
using lab::core::FrameContext;
using lab::render::DeviceFeatures;
using lab::render::GpuBuffer;
using lab::render::GpuImage;
using lab::render::GraphicsPipeline;
using lab::render::PipelineBuilder;
using lab::render::PipelineLayout;
using lab::render::Shader;
using lab::render::StagingUploader;

const char* MODELS[] = {"assets/DamagedHelmet/DamagedHelmet.gltf"};

// SPIR-V embedded at build time (glslc -mfmt=c, see CMakeLists).
const uint32_t MESH_VERT[] =
#include "mesh.vert.h"
    ;
const uint32_t MESH_FRAG[] =
#include "mesh.frag.h"
    ;

// Data types the lab owns; they encode the A0/B0 strategy.
struct GpuMesh {
    GpuBuffer vertex; // A0: no shared offsets — one buffer pair per mesh
    GpuBuffer index;
    uint32_t indexCount = 0;
    int material = -1; // index into m_materials (global), or -1
};
struct GpuMaterial {
    GpuImage baseColor; // set 1: bindings 0/1/2
    GpuImage metallicRoughness;
    GpuImage normal;
    GpuBuffer factors;                    // binding 3: MaterialUbo
    VkDescriptorSet set = VK_NULL_HANDLE; // B0: one set per material
};

// std140-compatible UBO layouts (match the shader blocks exactly).
struct FrameUbo {
    glm::mat4 viewProj{1.0f};
    glm::vec4 camPos{0.0f};
    glm::vec4 lightDir{0.0f};   // direction TO the light
    glm::vec4 lightColor{0.0f}; // rgb intensity
    glm::vec4 ambient{0.0f};
};
struct MaterialUbo {
    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 mr{1.0f}; // x = metallic, y = roughness
};
struct PushConstants {
    glm::mat4 model{1.0f}; // per object; viewProj lives in the frame UBO
};

class IndirectLab : public App {
public:
    IndirectLab()
        : App("lab_01 - indirect rendering",
              1280,
              720,
              DeviceFeatures{.multiDrawIndirect = true,
                             .drawIndirectCount = true,
                             .descriptorIndexing = true,
                             .shaderDrawParameters = true}) {

        // Order matters: layout before pipeline, geometry+materials before draw.
        loadModels();
        createMaterials();      // B0 material sets (set 1)
        createFrameResources(); // per-frame camera/light UBO (set 0)
        createPipeline();
        createScene();
        // Bench (GpuQueries/CsvReporter) not wired yet — see NOTE at file end.
    }

    ~IndirectLab() override {
        // GPU may still be using your buffers/pipeline; drain before members die.
        vkDeviceWaitIdle(m_context->getDevice());
        // m_sampler is a raw handle (no RAII wrapper) — destroy it explicitly.
        if (m_sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_context->getDevice(), m_sampler, nullptr);
        }
    }

protected:
    void onRender(const FrameContext& frame) override {
        VkCommandBuffer handle = frame.cmd.getHandle();

        // Color + depth to their attachment-optimal layouts. Both are discarded
        // each frame (contents not preserved), so a plain discard-then-clear
        // transition from UNDEFINED is fine.
        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        frame.cmd.transitionImageLayout(frame.depth,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_ASPECT_DEPTH_BIT);

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

        // Depth cleared to 1.0 (far); the pipeline's baked LESS test needs it.
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

        // Viewport/scissor are dynamic (baked so by PipelineBuilder), set per frame.
        VkViewport viewport{};
        viewport.width = static_cast<float>(frame.extent.width);
        viewport.height = static_cast<float>(frame.extent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(handle, 0, 1, &viewport);
        VkRect2D scissor{{0, 0}, frame.extent};
        vkCmdSetScissor(handle, 0, 1, &scissor);

        // Camera is sampled purely by frame index (deterministic fly-through).
        const float aspect =
            static_cast<float>(frame.extent.width) / static_cast<float>(frame.extent.height);
        const scene::CameraSample cam = scene::sampleCamera(m_path, m_frame, aspect);

        // Update this frame-in-flight's UBO (camera + a fixed directional light).
        const uint32_t slot = m_frame % App::FRAMES_IN_FLIGHT;
        FrameUbo fu;
        fu.viewProj = cam.proj * cam.view;
        fu.camPos = glm::vec4(cam.position, 1.0f);
        fu.lightDir = glm::normalize(glm::vec4(0.5f, 1.0f, 0.3f, 0.0f));
        fu.lightColor = glm::vec4(3.0f, 3.0f, 3.0f, 0.0f);
        fu.ambient = glm::vec4(0.03f, 0.03f, 0.03f, 0.0f);
        std::memcpy(m_frameUbo[slot].mapped(), &fu, sizeof(fu));

        // set 0 (frame) is bound once; set 1 (material) rebinds on change (B0).
        vkCmdBindDescriptorSets(handle,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeline.layout(),
                                0,
                                1,
                                &m_frameSet[slot],
                                0,
                                nullptr);

        // A0 × B0 draw loop. k=1 here, so every instance draws MODELS[0]'s meshes
        // (all of m_meshes). For k>1 you'd index meshes by instance.model; the
        // loop shape — bind-material-on-change, bind-per-mesh, push model, draw — is
        // the recording whose cost the A/B axes vary.
        //
        // ORDER: instances iterate in generation order. With a single material
        // that's fine, but the real study must sort the draw list by material (then
        // mesh) ONCE at setup — a fixed order identical across conditions — or B0
        // thrashes rebinds and A/B0/B1 stop being comparable (see ../README.md).
        m_pipeline.bind(handle);
        int boundMaterial = -1;
        for (const scene::Instance& inst : m_instances) {
            const PushConstants pc{glm::translate(glm::mat4(1.0f), inst.position) *
                                   glm::mat4_cast(inst.rotation)};

            for (const GpuMesh& mesh : m_meshes) {
                if (mesh.material >= 0 && mesh.material != boundMaterial) {
                    vkCmdBindDescriptorSets(handle,
                                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_pipeline.layout(),
                                            1,
                                            1,
                                            &m_materials[mesh.material].set,
                                            0,
                                            nullptr); // B0
                    boundMaterial = mesh.material;
                }
                const VkBuffer vertexBuffer = mesh.vertex.handle();
                const VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(handle, 0, 1, &vertexBuffer, &offset); // A0
                vkCmdBindIndexBuffer(handle, mesh.index.handle(), 0, VK_INDEX_TYPE_UINT32);
                vkCmdPushConstants(handle,
                                   m_pipeline.layout(),
                                   VK_SHADER_STAGE_VERTEX_BIT,
                                   0,
                                   sizeof(pc),
                                   &pc);
                vkCmdDrawIndexed(handle, mesh.indexCount, 1, 0, 0, 0);
            }
        }

        vkCmdEndRendering(handle);

        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        ++m_frame;

        // Bench still to wire: bracket the loop with GpuQueries (reset + timestamps
        // + pipeline-stats) and a CpuTimer; resolve the PREVIOUS frame's queries
        // here (resolve blocks on a signalled fence) and write a bench::Record via
        // CsvReporter; skip ~300 warm-up frames. See the NOTEs at the file end.
    }

private:
    // A0: load set M and give each mesh its own vertex/index buffer pair.
    void loadModels() {
        StagingUploader up(*m_context);
        for (const char* path : MODELS) {
            asset::CpuModel model = asset::loadModel(path);
            // Material indices in a CpuModel are model-local; offset them so they
            // index into the flattened m_materials across the whole set M.
            const int matBase = static_cast<int>(m_materials.size());

            for (const asset::MaterialData& md : model.materials) {
                // ponytail: assumes all three maps present (true for the model set
                // M so far). Add 1x1 default textures if a slot can be empty.
                if (md.baseColorTexture.empty() || md.metallicRoughnessTexture.empty() ||
                    md.normalTexture.empty()) {
                    throw std::runtime_error(std::string("material missing a PBR map in ") + path);
                }
                GpuMaterial mat;
                mat.baseColor = asset::loadImage2D(*m_context, md.baseColorTexture, /*srgb=*/true);
                mat.metallicRoughness =
                    asset::loadImage2D(*m_context, md.metallicRoughnessTexture, /*srgb=*/false);
                mat.normal = asset::loadImage2D(*m_context, md.normalTexture, /*srgb=*/false);

                MaterialUbo ubo;
                ubo.baseColorFactor = md.baseColorFactor;
                ubo.mr = glm::vec4(md.metallicFactor, md.roughnessFactor, 0.0f, 0.0f);
                mat.factors = GpuBuffer::createHostVisible(*m_context,
                                                           sizeof(MaterialUbo),
                                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
                std::memcpy(mat.factors.mapped(), &ubo, sizeof(ubo));

                m_materials.push_back(std::move(mat));
            }

            for (const asset::MeshData& mesh : model.meshes) {
                const VkDeviceSize vbytes = mesh.vertices.size() * sizeof(asset::Vertex);
                const VkDeviceSize ibytes = mesh.indices.size() * sizeof(uint32_t);
                GpuMesh gm;
                gm.vertex = GpuBuffer::createDeviceLocal(*m_context,
                                                         vbytes,
                                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                gm.index = GpuBuffer::createDeviceLocal(*m_context,
                                                        ibytes,
                                                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
                up.upload(gm.vertex, mesh.vertices.data(), vbytes);
                up.upload(gm.index, mesh.indices.data(), ibytes);
                gm.indexCount = static_cast<uint32_t>(mesh.indices.size());
                gm.material = mesh.material < 0 ? -1 : matBase + mesh.material;
                m_meshes.push_back(std::move(gm));
            }
        }
        up.flush(); // one blocking submit for all mesh copies
    }

    // B0: set 1 per material = baseColor/metalRough/normal samplers + factors UBO.
    void createMaterials() {
        // Sampler (mip/anisotropy = a fixed experiment control, owned by the lab).
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.maxLod = VK_LOD_CLAMP_NONE;
        VK_CHECK(vkCreateSampler(m_context->getDevice(), &si, nullptr, &m_sampler));

        constexpr VkDescriptorType SAMPLER = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        m_materialLayout =
            render::DescriptorSetLayout::Builder(*m_context)
                .binding(0, SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .binding(1, SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .binding(2, SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT)
                .build();

        // Pool covers material sets (3 samplers + 1 UBO each) plus the per-frame
        // UBO sets allocated by createFrameResources().
        const uint32_t mats = static_cast<uint32_t>(m_materials.size());
        std::vector<VkDescriptorPoolSize> sizes = {
            {SAMPLER, 3 * mats},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, mats + App::FRAMES_IN_FLIGHT}};
        m_pool = render::DescriptorPool::create(*m_context, mats + App::FRAMES_IN_FLIGHT, sizes);

        for (GpuMaterial& mat : m_materials) {
            mat.set = m_pool.allocate(m_materialLayout.handle());
            const auto image = [&](VkImageView view) {
                VkDescriptorImageInfo info{};
                info.sampler = m_sampler;
                info.imageView = view;
                info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                return info;
            };
            VkDescriptorBufferInfo factors{mat.factors.handle(), 0, sizeof(MaterialUbo)};
            render::DescriptorWriter(*m_context)
                .writeImage(mat.set, 0, SAMPLER, image(mat.baseColor.view()))
                .writeImage(mat.set, 1, SAMPLER, image(mat.metallicRoughness.view()))
                .writeImage(mat.set, 2, SAMPLER, image(mat.normal.view()))
                .writeBuffer(mat.set, 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, factors)
                .flush();
        }
    }

    // set 0: per-frame camera + light UBO, one buffer/set per frame-in-flight so
    // the CPU can write the next frame while the GPU reads the current one.
    void createFrameResources() {
        m_frameLayout = render::DescriptorSetLayout::Builder(*m_context)
                            .binding(0,
                                     VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                            .build();

        for (uint32_t i = 0; i < App::FRAMES_IN_FLIGHT; ++i) {
            m_frameUbo[i] = GpuBuffer::createHostVisible(*m_context,
                                                         sizeof(FrameUbo),
                                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            m_frameSet[i] = m_pool.allocate(m_frameLayout.handle());
            VkDescriptorBufferInfo info{m_frameUbo[i].handle(), 0, sizeof(FrameUbo)};
            render::DescriptorWriter(*m_context)
                .writeBuffer(m_frameSet[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, info)
                .flush();
        }
    }

    // Pipeline: set 0 = frame UBO, set 1 = material; per-object model push constant.
    void createPipeline() {
        Shader vert = Shader::fromSpirv(*m_context, MESH_VERT, sizeof(MESH_VERT));
        Shader frag = Shader::fromSpirv(*m_context, MESH_FRAG, sizeof(MESH_FRAG));

        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pcRange.size = sizeof(PushConstants);
        m_pipelineLayout =
            PipelineLayout::create(*m_context,
                                   {m_frameLayout.handle(), m_materialLayout.handle()},
                                   {pcRange});

        asset::VertexInputDesc vin = asset::standardVertexInput();
        m_pipeline = PipelineBuilder(*m_context)
                         .shaders(vert.handle(), frag.handle())
                         .layout(m_pipelineLayout)
                         .vertexInput(vin.bindings, vin.attributes)
                         .build();
    }

    // Deterministic instances (dumped to JSON) + a camera loop framing them.
    // N is small for bring-up; the study sweeps it.
    void createScene() {
        scene::SceneParams params{};
        params.seed = 42;
        params.count = 25;     // N
        params.modelCount = 1; // k = |MODELS|
        params.spacing = 3.0f; // DamagedHelmet is ~2 units across
        params.jitter = 0.3f;
        m_instances = scene::generateScene(params);
        scene::dumpSceneJson(m_instances, params, "scene.json");

        // Frame the whole grid: sweep radius/height from its half-extent.
        const glm::vec3 center = scene::sceneCenter(m_instances);
        const glm::vec3 half = scene::sceneHalfExtent(m_instances);
        const float span = std::max(half.x, half.z) + 3.0f;
        m_path = scene::makeSweepPath(center, span * 1.5f, span);
    }

    // Declaration order matters for destruction (reverse): pipeline before its
    // layout, sets before their pool. All borrow the base App's Context.
    std::vector<GpuMesh> m_meshes;
    std::vector<GpuMaterial> m_materials;
    VkSampler m_sampler{VK_NULL_HANDLE}; // raw; destroyed in ~IndirectLab
    render::DescriptorSetLayout m_materialLayout;
    render::DescriptorSetLayout m_frameLayout;
    render::DescriptorPool m_pool;
    std::array<GpuBuffer, App::FRAMES_IN_FLIGHT> m_frameUbo;         // host-visible
    std::array<VkDescriptorSet, App::FRAMES_IN_FLIGHT> m_frameSet{}; // from m_pool
    PipelineLayout m_pipelineLayout;
    GraphicsPipeline m_pipeline;

    std::vector<scene::Instance> m_instances;
    scene::CameraPath m_path;

    // Bench members to add later: std::optional<bench::GpuQueries> (no default
    // ctor), std::optional<bench::CsvReporter>, bench::CpuTimer.
    uint64_t m_frame{0};
};

// NOTE — CPU submit-span: App::drawFrame owns submit+present, so onRender can
// only time command RECORDING. To measure the submit span you must either add a
// hook to App (template change) or accept recording-only CPU cost for A0. Record
// which you chose in the CSV/notes.
//
// NOTE — present mode: swapchain is still FIFO (vsync). Removing vsync
// (VK_PRESENT_MODE_IMMEDIATE_KHR opt-in) is the one template change this lab
// needs before timing numbers are meaningful — see ../README.md.

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

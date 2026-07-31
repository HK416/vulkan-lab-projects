// lab_06 — GPU-driven indirect rendering study. Condition A2 × B1.
//
//   A2 = ONE vertex buffer and ONE index buffer holding every mesh. A mesh is
//        selected by the firstIndex/vertexOffset fields of its indirect command,
//        not by rebinding buffers — so the buffers are bound once per frame and
//        each object gets its own vkCmdDrawIndexedIndirect (drawCount = 1).
//   B1 = bindless: ONE descriptor set bound per frame, materials indexed from a
//        storage buffer. Identical to lab_02/lab_04, so lab_05 vs lab_06 isolates
//        B0 vs B1 under A2.
//
// lab_03 vs lab_05 changes buffer strategy AND draw granularity at once, so the
// design forbids a standalone conclusion from that pair (see section 1). The
// pair A2 vs A3 (lab_05/06 vs lab_07) is the one that isolates multi-draw
// batching, since both sides use the single buffer.
//
// The harness lives in template (lab::asset / scene / bench / render). This file
// only WIRES it into the A2 × B1 strategy. Design + data flow: ./README.md and
// ../indirect-rendering-experiment.md.
//

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <string>
#include <tuple>
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
#include "core/env.h"
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

// Model set M. Identical list and order in every lab — k and the material count
// it implies are experiment controls, not a per-lab choice.
const char* const MODELS[] = {
    "assets/Avocado/Avocado.gltf",
    "assets/BoomBox/BoomBox.gltf",
    "assets/Corset/Corset.gltf",
    "assets/DamagedHelmet/DamagedHelmet.gltf",
    "assets/FlightHelmet/FlightHelmet.gltf", // 6 materials / 6 meshes
    "assets/Lantern/Lantern.gltf",           // 3 meshes
    "assets/WaterBottle/WaterBottle.gltf",
};
constexpr uint32_t MODEL_COUNT = static_cast<uint32_t>(std::size(MODELS)); // k = 7

// Every model is placed at this world size (longest AABB axis). The set spans
// ~0.07 to ~5 authored units; without this, per-model fragment load would vary
// by orders of magnitude and the resolution axis would measure nothing.
constexpr float NORMALIZED_SIZE = 2.0f;

// SPIR-V embedded at build time (glslc -mfmt=c, see CMakeLists).
const uint32_t MESH_VERT[] =
#include "mesh.vert.h"
    ;
const uint32_t MESH_FRAG[] =
#include "mesh.frag.h"
    ;

// Data types the lab owns; they encode the A2/B1 strategy.
//
// A2: a mesh is no longer a pair of buffers, it is a RANGE inside the two shared
// buffers. Exactly these three numbers go into the indirect command, which is
// how mesh selection stops costing a bind.
struct GpuMesh {
    uint32_t indexCount = 0;
    uint32_t firstIndex = 0;  // offset into the shared index buffer
    int32_t vertexOffset = 0; // added to every index, into the shared vertex buffer
    int material = -1;        // index into m_materials (global), or -1
};
struct GpuMaterial {
    GpuImage baseColor;         // B1: no descriptor set of its own — the three
    GpuImage metallicRoughness; //   views go into the single bindless texture
    GpuImage normal;            //   array, the factors into the materials SSBO.
};
// Which slice of m_meshes belongs to one model in M, plus the recenter+rescale
// that puts it on the common size. Instances draw only their own model's meshes.
struct ModelRange {
    uint32_t firstMesh = 0;
    uint32_t meshCount = 0;
    glm::mat4 normalize{1.0f};
};
// One recorded draw. Its index in m_draws is the firstInstance the indirect
// command carries, and therefore the shader's index into the objects SSBO.
struct Draw {
    uint32_t mesh = 0;
    int material = -1;
    glm::mat4 model{1.0f};
};

// std140-compatible UBO layouts (match the shader blocks exactly).
struct FrameUbo {
    glm::mat4 viewProj{1.0f};
    glm::vec4 camPos{0.0f};
    glm::vec4 lightDir{0.0f};   // direction TO the light
    glm::vec4 lightColor{0.0f}; // rgb intensity
    glm::vec4 ambient{0.0f};
};
// std430 storage-buffer layout, byte-identical to lab_02/lab_04's.
struct MaterialGpu {
    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 mr{1.0f};         // x = metallic, y = roughness
    glm::uvec4 tex{0, 0, 0, 0}; // x/y/z = index into the bindless textures[]
};
// std430 storage-buffer layout, byte-identical to the other labs' — the vertex
// shader that reads it is shared across every condition.
struct ObjectGpu {
    glm::mat4 model{1.0f};
    glm::uvec4 material{0, 0, 0, 0}; // x = index into materials[]; unused under B0
};

class IndirectLab : public App {
public:
    IndirectLab()
        : App("lab_06 - indirect rendering",
              1280,
              720,
              // Only what A2 × B1 needs (design section 7). NOT multiDrawIndirect:
              // A2 issues drawCount = 1 per call, which is allowed without it —
              // that feature is what separates A3 from A2.
              DeviceFeatures{.drawIndirectFirstInstance = true,
                             .descriptorIndexing = true,
                             .shaderDrawParameters = true},
              /*uncappedPresent=*/true) { // no vsync — GPU time must be unclamped

        const DeviceFeatures& have = m_context->enabledFeatures();
        if (!have.drawIndirectFirstInstance) {
            throw std::runtime_error("A2 index path needs drawIndirectFirstInstance");
        }
        if (!have.shaderDrawParameters) {
            throw std::runtime_error("A2 index path needs shaderDrawParameters (gl_BaseInstance)");
        }
        if (!have.descriptorIndexing) {
            throw std::runtime_error("B1 needs descriptorIndexing; device does not support it");
        }

        // Order matters: scene before the object/indirect buffers, layouts before
        // the pipeline.
        loadModels();
        createScene();
        createBindless();       // B1: the one set (set 1) — textures + SSBO
        createFrameResources(); // set 0: frame UBO + objects SSBO
        createIndirect();       // A2: the static indirect buffer
        createPipeline();
        createBench();
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

        const uint32_t slot = m_frame % App::FRAMES_IN_FLIGHT;

        // Resolve the frame that last used this slot (FRAMES_IN_FLIGHT ago). Its
        // fence has since signalled, so the queries are ready and reading them
        // does not block. Doing it now — a frame late — avoids stalling the GPU.
        if (m_frame >= App::FRAMES_IN_FLIGHT) {
            recordMeasurement(slot, m_frame - App::FRAMES_IN_FLIGHT, frame.extent);
        }

        // CPU record timer + GPU query bracket. reset and the begin timestamp must
        // be OUTSIDE dynamic rendering; the pipeline-stats query goes inside it.
        m_recordTimer.start();
        m_queries[slot].reset(frame.cmd);
        m_queries[slot].writeBeginTimestamp(frame.cmd);

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
        FrameUbo fu;
        fu.viewProj = cam.proj * cam.view;
        fu.camPos = glm::vec4(cam.position, 1.0f);
        fu.lightDir = glm::normalize(glm::vec4(0.5f, 1.0f, 0.3f, 0.0f));
        fu.lightColor = glm::vec4(3.0f, 3.0f, 3.0f, 0.0f);
        fu.ambient = glm::vec4(0.03f, 0.03f, 0.03f, 0.0f);
        std::memcpy(m_frameUbo[slot].mapped(), &fu, sizeof(fu));

        // B1: set 0 (frame + objects) and set 1 (bindless) bound ONCE. Nothing
        // rebinds inside the draw loop — that absence is the point of B1.
        const std::array<VkDescriptorSet, 2> sets{m_frameSet[slot], m_bindlessSet};
        vkCmdBindDescriptorSets(handle,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipeline.layout(),
                                0,
                                static_cast<uint32_t>(sets.size()),
                                sets.data(),
                                0,
                                nullptr);

        m_pipeline.bind(handle);

        // A2: the geometry buffers are bound ONCE for the whole frame. Everything
        // a draw needs to find its mesh travels in the indirect command instead.
        const VkBuffer vertexBuffer = m_vertexBuffer.handle();
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(handle, 0, 1, &vertexBuffer, &offset);
        vkCmdBindIndexBuffer(handle, m_indexBuffer.handle(), 0, VK_INDEX_TYPE_UINT32);

        // A2 × B1 draw loop: one indirect command per object, drawCount = 1, and
        // nothing else. No buffer binds, no descriptor binds — the loop body is a
        // single Vulkan call. What remains is pure submission cost, which is
        // exactly what A3 removes by folding these into one multi-draw.
        m_queries[slot].beginPipelineStats(frame.cmd); // inside rendering
        for (uint32_t i = 0; i < m_draws.size(); ++i) {
            vkCmdDrawIndexedIndirect(handle,
                                     m_indirectBuffer.handle(),
                                     i * sizeof(VkDrawIndexedIndirectCommand),
                                     1, // A2: per object, so no multiDrawIndirect needed
                                     sizeof(VkDrawIndexedIndirectCommand));
        }
        m_queries[slot].endPipelineStats(frame.cmd); // before end-rendering

        vkCmdEndRendering(handle);

        m_queries[slot].writeEndTimestamp(frame.cmd); // outside rendering

        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        m_recordMs[slot] = m_recordTimer.stopMilliseconds();
        ++m_frame;
    }

    // Resolve one slot's queries and (past warm-up) append a CSV row. cpuSubmitMs
    // stays 0: App owns submit, so onRender can only time recording (see NOTE).
    void recordMeasurement(uint32_t slot, uint64_t doneFrame, VkExtent2D extent) {
        const double gpuMs = m_queries[slot].resolveGpuMilliseconds();
        const bench::GpuQueries::PipelineStats stats = m_queries[slot].resolvePipelineStats();
        if (doneFrame < WARMUP_FRAMES) {
            return; // discard warm-up frames
        }
        bench::Record r;
        r.condition = "A2xB1";
        r.width = extent.width;
        r.height = extent.height;
        r.objectCount = static_cast<uint32_t>(m_instances.size());
        r.gpuMs = gpuMs;
        r.cpuRecordMs = m_recordMs[slot];
        r.triangles = stats.inputAssemblyPrimitives;
        r.vertexInvocations = stats.vertexShaderInvocations;
        r.fragmentInvocations = stats.fragmentShaderInvocations;
        m_reporter.write(r);
    }

private:
    // A2: concatenate every mesh into ONE vertex buffer and ONE index buffer.
    //
    // Two passes, because the buffer sizes are only known once every model is
    // loaded: pass 1 loads the CPU data, creates the material images and records
    // each mesh's offsets; pass 2 creates the two buffers and copies each mesh to
    // its offset.
    //
    // Indices stay mesh-local (0-based) — vertexOffset in the indirect command
    // rebases them, which is precisely what that field exists for. No index
    // rewriting, so the index data is byte-identical to the A0/A1 labs' and the
    // vertex/triangle counts cannot drift between conditions.
    void loadModels() {
        std::vector<asset::CpuModel> loaded;
        loaded.reserve(MODEL_COUNT);
        uint32_t vertexTotal = 0;
        uint32_t indexTotal = 0;

        for (const char* path : MODELS) {
            asset::CpuModel model = asset::loadModel(path);
            ModelRange range;
            range.firstMesh = static_cast<uint32_t>(m_meshes.size());
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
                // B1: factors go to the shared SSBO, not a per-material UBO. The
                // texture slots are this material's three consecutive entries in
                // the bindless array.
                const uint32_t base = 3 * static_cast<uint32_t>(m_materials.size());
                MaterialGpu mg;
                mg.baseColorFactor = md.baseColorFactor;
                mg.mr = glm::vec4(md.metallicFactor, md.roughnessFactor, 0.0f, 0.0f);
                mg.tex = glm::uvec4(base, base + 1, base + 2, 0);
                m_materialData.push_back(mg);

                GpuMaterial mat;
                mat.baseColor = asset::loadImage2D(*m_context, md.baseColorTexture, /*srgb=*/true);
                mat.metallicRoughness =
                    asset::loadImage2D(*m_context, md.metallicRoughnessTexture, /*srgb=*/false);
                mat.normal = asset::loadImage2D(*m_context, md.normalTexture, /*srgb=*/false);
                m_materials.push_back(std::move(mat));
            }

            for (const asset::MeshData& mesh : model.meshes) {
                GpuMesh gm;
                gm.indexCount = static_cast<uint32_t>(mesh.indices.size());
                gm.firstIndex = indexTotal;
                gm.vertexOffset = static_cast<int32_t>(vertexTotal);
                gm.material = mesh.material < 0 ? -1 : matBase + mesh.material;
                m_meshes.push_back(gm);

                vertexTotal += static_cast<uint32_t>(mesh.vertices.size());
                indexTotal += static_cast<uint32_t>(mesh.indices.size());
            }

            // Recenter on the AABB, then scale the longest axis to
            // NORMALIZED_SIZE. Applied before the instance transform, so every
            // model contributes the same silhouette area regardless of how it
            // was authored.
            range.meshCount = static_cast<uint32_t>(m_meshes.size()) - range.firstMesh;
            const glm::vec3 extent = model.max - model.min;
            const float longest = std::max({extent.x, extent.y, extent.z});
            const float scale = longest > 0.0f ? NORMALIZED_SIZE / longest : 1.0f;
            range.normalize = glm::scale(glm::mat4(1.0f), glm::vec3(scale)) *
                              glm::translate(glm::mat4(1.0f), -0.5f * (model.min + model.max));
            m_models.push_back(range);

            loaded.push_back(std::move(model));
        }

        // Pass 2: two allocations for the whole scene, versus 2 per mesh in A0/A1.
        // That difference is one of the things the memory metric records.
        m_vertexBuffer =
            GpuBuffer::createDeviceLocal(*m_context,
                                         VkDeviceSize{vertexTotal} * sizeof(asset::Vertex),
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        m_indexBuffer = GpuBuffer::createDeviceLocal(*m_context,
                                                     VkDeviceSize{indexTotal} * sizeof(uint32_t),
                                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        StagingUploader up(*m_context);
        uint32_t mesh = 0;
        for (const asset::CpuModel& model : loaded) {
            for (const asset::MeshData& md : model.meshes) {
                const GpuMesh& gm = m_meshes[mesh++];
                up.upload(m_vertexBuffer,
                          md.vertices.data(),
                          md.vertices.size() * sizeof(asset::Vertex),
                          VkDeviceSize{static_cast<uint32_t>(gm.vertexOffset)} *
                              sizeof(asset::Vertex));
                up.upload(m_indexBuffer,
                          md.indices.data(),
                          md.indices.size() * sizeof(uint32_t),
                          VkDeviceSize{gm.firstIndex} * sizeof(uint32_t));
            }
        }
        up.flush(); // one blocking submit for all mesh copies
        spdlog::info("A2 single buffer: {} vertices, {} indices", vertexTotal, indexTotal);
    }

    // Deterministic instances (dumped to JSON) + a camera loop framing them, then
    // the flattened, sorted draw list every frame replays.
    void createScene() {
        scene::SceneParams params{};
        params.seed = 42;
        // N — the object-count axis. LAB_OBJECTS picks a level without a rebuild;
        // the design sweeps {128, 512, 2048, 8192, 32768}.
        params.count = static_cast<uint32_t>(lab::core::envInt("LAB_OBJECTS", 128));
        params.modelCount = MODEL_COUNT; // k
        params.spacing = 3.0f;           // every model is NORMALIZED_SIZE across
        params.jitter = 0.3f;
        m_instances = scene::generateScene(params);
        scene::dumpSceneJson(m_instances, params, "scene.json");

        // Frame the whole grid: sweep radius/height from its half-extent.
        const glm::vec3 center = scene::sceneCenter(m_instances);
        const glm::vec3 half = scene::sceneHalfExtent(m_instances);
        const float span = std::max(half.x, half.z) + 3.0f;
        m_path = scene::makeSweepPath(center, span * 1.5f, span);

        // Flatten instance × its own model's meshes. Transforms are static, so
        // they are computed once here rather than per frame.
        for (const scene::Instance& inst : m_instances) {
            const ModelRange& range = m_models[inst.model];
            const glm::mat4 model = glm::translate(glm::mat4(1.0f), inst.position) *
                                    glm::mat4_cast(inst.rotation) * range.normalize;
            for (uint32_t i = 0; i < range.meshCount; ++i) {
                const uint32_t mesh = range.firstMesh + i;
                m_draws.push_back(Draw{mesh, m_meshes[mesh].material, model});
            }
        }

        // THE render order, shared by every condition: material then mesh.
        // stable_sort keeps instance generation order inside a group, fixing the
        // overdraw pattern. A2 does not need the grouping to draw, but B0 rebinds
        // on material change and the order is a fixed control either way.
        std::stable_sort(m_draws.begin(), m_draws.end(), [](const Draw& a, const Draw& b) {
            return std::tie(a.material, a.mesh) < std::tie(b.material, b.mesh);
        });

        // objects[] is indexed by draw index, so it is built AFTER the sort.
        m_objectData.reserve(m_draws.size());
        for (const Draw& draw : m_draws) {
            ObjectGpu obj;
            obj.model = draw.model;
            obj.material.x = static_cast<uint32_t>(std::max(draw.material, 0));
            m_objectData.push_back(obj);
        }
        spdlog::info("scene: N={} k={} meshes={} materials={} draws={}",
                     m_instances.size(),
                     MODEL_COUNT,
                     m_meshes.size(),
                     m_materials.size(),
                     m_draws.size());
    }

    // B1: ONE descriptor set (set 1) holding every material — a texture array
    // plus the materials SSBO. Nothing here is rebound per draw. Byte-identical
    // to lab_02/lab_04's, so B1 does not drift between the A conditions.
    void createBindless() {
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

        // Separate image and sampler descriptors. Combined ones would consume a
        // sampler slot per texture, and Apple/Metal caps per-stage samplers at 16
        // — the array is 3 x material count. One shared sampler is also the
        // experiment's fixed filtering control.
        constexpr VkDescriptorType IMAGE = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        constexpr VkDescriptorType SAMPLER = VK_DESCRIPTOR_TYPE_SAMPLER;
        constexpr VkDescriptorType SSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        const uint32_t textureCount = 3 * static_cast<uint32_t>(m_materials.size());

        // Device-local: the shader reads this every fragment, so keep it off the
        // host heap or GPU time measures PCIe, not the draw strategy.
        m_materialBuffer = GpuBuffer::createDeviceLocal(*m_context,
                                                        m_materialData.size() * sizeof(MaterialGpu),
                                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        render::uploadDeviceLocal(*m_context,
                                  m_materialBuffer,
                                  m_materialData.data(),
                                  m_materialBuffer.size());

        // The array is sized exactly (no PARTIALLY_BOUND / UPDATE_AFTER_BIND):
        // every slot is written once at setup and never changes. Culling (exp 2)
        // is what will need those flags.
        m_bindlessLayout = render::DescriptorSetLayout::Builder(*m_context)
                               .binding(0, SSBO, VK_SHADER_STAGE_FRAGMENT_BIT)
                               .binding(1, IMAGE, VK_SHADER_STAGE_FRAGMENT_BIT, textureCount)
                               .binding(2, SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
                               .build();

        // Pool covers the one bindless set plus the per-frame sets allocated by
        // createFrameResources() (UBO + the shared objects SSBO).
        std::vector<VkDescriptorPoolSize> sizes = {
            {IMAGE, textureCount},
            {SAMPLER, 1},
            {SSBO, 1 + App::FRAMES_IN_FLIGHT},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, App::FRAMES_IN_FLIGHT}};
        m_pool = render::DescriptorPool::create(*m_context, 1 + App::FRAMES_IN_FLIGHT, sizes);
        m_bindlessSet = m_pool.allocate(m_bindlessLayout.handle());

        const auto image = [](VkImageView view) {
            VkDescriptorImageInfo info{};
            info.imageView = view; // the sampler is its own descriptor now
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return info;
        };
        VkDescriptorImageInfo samplerInfo{};
        samplerInfo.sampler = m_sampler;

        VkDescriptorBufferInfo materials{m_materialBuffer.handle(), 0, m_materialBuffer.size()};
        render::DescriptorWriter writer(*m_context);
        writer.writeBuffer(m_bindlessSet, 0, SSBO, materials)
            .writeImage(m_bindlessSet, 2, SAMPLER, samplerInfo);
        for (uint32_t i = 0; i < m_materials.size(); ++i) {
            // Three consecutive array elements per material — the layout that
            // MaterialGpu::tex was built against in loadModels().
            writer
                .writeImage(m_bindlessSet, 1, IMAGE, image(m_materials[i].baseColor.view()), 3 * i)
                .writeImage(m_bindlessSet,
                            1,
                            IMAGE,
                            image(m_materials[i].metallicRoughness.view()),
                            3 * i + 1)
                .writeImage(m_bindlessSet,
                            1,
                            IMAGE,
                            image(m_materials[i].normal.view()),
                            3 * i + 2);
        }
        writer.flush();
    }

    // set 0: per-frame camera + light UBO (one buffer/set per frame-in-flight, so
    // the CPU can write the next frame while the GPU reads the current one) plus
    // the static objects SSBO. The objects buffer belongs here — not in the
    // material set — because it is identical in every condition, which is what
    // keeps the vertex shader byte-identical across A0-A3 x B0/B1.
    void createFrameResources() {
        m_objectBuffer = GpuBuffer::createDeviceLocal(*m_context,
                                                      m_objectData.size() * sizeof(ObjectGpu),
                                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        render::uploadDeviceLocal(*m_context,
                                  m_objectBuffer,
                                  m_objectData.data(),
                                  m_objectBuffer.size());

        m_frameLayout =
            render::DescriptorSetLayout::Builder(*m_context)
                .binding(0,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                .binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_VERTEX_BIT)
                .build();

        VkDescriptorBufferInfo objects{m_objectBuffer.handle(), 0, m_objectBuffer.size()};
        for (uint32_t i = 0; i < App::FRAMES_IN_FLIGHT; ++i) {
            m_frameUbo[i] = GpuBuffer::createHostVisible(*m_context,
                                                         sizeof(FrameUbo),
                                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            m_frameSet[i] = m_pool.allocate(m_frameLayout.handle());
            VkDescriptorBufferInfo info{m_frameUbo[i].handle(), 0, sizeof(FrameUbo)};
            render::DescriptorWriter(*m_context)
                .writeBuffer(m_frameSet[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, info)
                .writeBuffer(m_frameSet[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, objects)
                .flush();
        }
    }

    // A2: one indirect command per draw, index-aligned with m_draws (and hence
    // with objects[]). Unlike A0/A1 the commands carry firstIndex/vertexOffset,
    // which is what lets the shared buffers stay bound. Culling is off in
    // experiment 1, so this is built once and costs 0 per frame.
    void createIndirect() {
        std::vector<VkDrawIndexedIndirectCommand> commands;
        commands.reserve(m_draws.size());
        for (uint32_t i = 0; i < m_draws.size(); ++i) {
            const GpuMesh& mesh = m_meshes[m_draws[i].mesh];
            VkDrawIndexedIndirectCommand cmd{};
            cmd.indexCount = mesh.indexCount;
            cmd.instanceCount = 1;
            cmd.firstIndex = mesh.firstIndex;     // A2: where this mesh starts
            cmd.vertexOffset = mesh.vertexOffset; // A2: rebases its local indices
            cmd.firstInstance = i;                // -> gl_BaseInstance -> objects[i]
            commands.push_back(cmd);
        }
        const VkDeviceSize bytes = commands.size() * sizeof(VkDrawIndexedIndirectCommand);
        m_indirectBuffer =
            GpuBuffer::createDeviceLocal(*m_context, bytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        render::uploadDeviceLocal(*m_context, m_indirectBuffer, commands.data(), bytes);
    }

    // Pipeline: set 0 = frame UBO + objects, set 1 = material. No push constants
    // — log the SPIR-V hashes so the shared vertex shader can be shown identical.
    void createPipeline() {
        Shader vert = Shader::fromSpirv(*m_context, MESH_VERT, sizeof(MESH_VERT));
        Shader frag = Shader::fromSpirv(*m_context, MESH_FRAG, sizeof(MESH_FRAG));
        spdlog::info("spirv mesh.vert={} mesh.frag={}", vert.hashHex(), frag.hashHex());

        m_pipelineLayout =
            PipelineLayout::create(*m_context, {m_frameLayout.handle(), m_bindlessLayout.handle()});

        asset::VertexInputDesc vin = asset::standardVertexInput();
        m_pipeline = PipelineBuilder(*m_context)
                         .shaders(vert.handle(), frag.handle())
                         .layout(m_pipelineLayout)
                         .vertexInput(vin.bindings, vin.attributes)
                         .build();
    }

    // One GpuQueries per frame-in-flight so a frame's queries are only read after
    // its fence has signalled (see recordMeasurement). Environment dumped once.
    void createBench() {
        m_queries.reserve(App::FRAMES_IN_FLIGHT);
        for (uint32_t i = 0; i < App::FRAMES_IN_FLIGHT; ++i) {
            m_queries.emplace_back(*m_context);
        }
        bench::dumpEnvironmentJson(*m_context, "env.json");
        if (!m_queries[0].gpuSupported()) {
            spdlog::warn("GPU timestamps unsupported on this device; gpuMs will be 0");
        }
    }

    // Declaration order matters for destruction (reverse): pipeline before its
    // layout, sets before their pool. All borrow the base App's Context.
    std::vector<GpuMesh> m_meshes;    // ranges into the two buffers below
    GpuBuffer m_vertexBuffer;         // A2: one for the whole model set
    GpuBuffer m_indexBuffer;          // A2: one for the whole model set
    std::vector<ModelRange> m_models; // index-aligned with MODELS
    std::vector<GpuMaterial> m_materials;
    VkSampler m_sampler{VK_NULL_HANDLE}; // raw; destroyed in ~IndirectLab
    std::vector<MaterialGpu> m_materialData;
    std::vector<ObjectGpu> m_objectData;
    GpuBuffer m_materialBuffer; // set 1, binding 0
    GpuBuffer m_objectBuffer;   // set 0, binding 1 — same place in every condition
    GpuBuffer m_indirectBuffer; // A2: static, one command per draw
    render::DescriptorSetLayout m_bindlessLayout;
    render::DescriptorSetLayout m_frameLayout;
    render::DescriptorPool m_pool;
    VkDescriptorSet m_bindlessSet{VK_NULL_HANDLE};                   // B1: exactly one
    std::array<GpuBuffer, App::FRAMES_IN_FLIGHT> m_frameUbo;         // host-visible
    std::array<VkDescriptorSet, App::FRAMES_IN_FLIGHT> m_frameSet{}; // from m_pool
    PipelineLayout m_pipelineLayout;
    GraphicsPipeline m_pipeline;

    std::vector<scene::Instance> m_instances;
    std::vector<Draw> m_draws; // index == objects[] index == firstInstance
    scene::CameraPath m_path;

    // Bench: measurements start after this many frames (warm-up discarded).
    static constexpr uint64_t WARMUP_FRAMES = 300;
    std::vector<bench::GpuQueries> m_queries; // one per frame-in-flight
    std::array<double, App::FRAMES_IN_FLIGHT> m_recordMs{};
    bench::CsvReporter m_reporter{"results.csv"};
    bench::CpuTimer m_recordTimer;

    uint64_t m_frame{0};
};

// NOTE — CPU submit-span: App::drawFrame owns submit+present, so onRender can
// only time command RECORDING. Same limitation as every other lab.
//
// NOTE — A1 vs A2 changes buffer strategy AND draw granularity together, so the
// design forbids a standalone conclusion from that pair. Use A0 vs A1 for the
// direct-vs-indirect question and A2 vs A3 for the batching question.

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

// lab_04 — GPU-driven indirect rendering study. Condition A1 × B1.
//
//   A1 = multi-buffer (one VkBuffer pair per mesh, same as A0) but ONE
//        vkCmdDrawIndexedIndirect per mesh buffer, covering every object that
//        uses that mesh. The indirect granularity is the buffer, not the object.
//        Identical to lab_03, so lab_03 vs lab_04 isolates B0 vs B1 under A1.
//   B1 = bindless: ONE descriptor set bound per frame, materials indexed from a
//        storage buffer. Identical to lab_02, so lab_02 vs lab_04 isolates
//        A0 vs A1 under B1.
//
// The harness lives in template (lab::asset / scene / bench / render). This file
// only WIRES it into the A1 × B1 strategy. Design + data flow: ./README.md and
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

// Data types the lab owns; they encode the A1/B0 strategy.
struct GpuMesh {
    GpuBuffer vertex; // A1: still one buffer pair per mesh — only the draw differs
    GpuBuffer index;
    uint32_t indexCount = 0;
    int material = -1; // index into m_materials (global), or -1
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
// A1's unit of submission: every draw of one mesh, as a contiguous run of
// indirect commands issued by a single vkCmdDrawIndexedIndirect.
struct MeshBatch {
    uint32_t mesh = 0;
    int material = -1;
    uint32_t firstDraw = 0; // index into the indirect buffer, in commands
    uint32_t drawCount = 0;
};

// std140-compatible UBO layouts (match the shader blocks exactly).
struct FrameUbo {
    glm::mat4 viewProj{1.0f};
    glm::vec4 camPos{0.0f};
    glm::vec4 lightDir{0.0f};   // direction TO the light
    glm::vec4 lightColor{0.0f}; // rgb intensity
    glm::vec4 ambient{0.0f};
};
// std430 storage-buffer layout, byte-identical to lab_02's.
struct MaterialGpu {
    glm::vec4 baseColorFactor{1.0f};
    glm::vec4 mr{1.0f};         // x = metallic, y = roughness
    glm::uvec4 tex{0, 0, 0, 0}; // x/y/z = index into the bindless textures[]
};
// std430 storage-buffer layout, byte-identical to lab_02's — the vertex shader
// that reads it is shared across every indirect condition.
struct ObjectGpu {
    glm::mat4 model{1.0f};
    glm::uvec4 material{0, 0, 0, 0}; // x = index into materials[]; unused under B0
};

class IndirectLab : public App {
public:
    IndirectLab()
        : App("lab_04 - indirect rendering",
              1280,
              720,
              // Only what A1 × B1 needs (design section 7): indirect draws with
              // a per-command firstInstance, gl_BaseInstance to read it back, and
              // descriptor indexing for the bindless material array.
              DeviceFeatures{.multiDrawIndirect = true,
                             .drawIndirectFirstInstance = true,
                             .descriptorIndexing = true,
                             .shaderDrawParameters = true},
              /*uncappedPresent=*/true) { // no vsync — GPU time must be unclamped

        // A1 is not emulatable: without these the indirect path either cannot
        // batch or cannot carry the object index. Fail loudly rather than
        // silently measuring a different condition.
        const DeviceFeatures& have = m_context->enabledFeatures();
        if (!have.multiDrawIndirect) {
            throw std::runtime_error("A1 needs multiDrawIndirect; device does not support it");
        }
        if (!have.drawIndirectFirstInstance) {
            throw std::runtime_error("A1 index path needs drawIndirectFirstInstance");
        }
        if (!have.shaderDrawParameters) {
            throw std::runtime_error("A1 index path needs shaderDrawParameters (gl_BaseInstance)");
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
        createIndirect();       // A1: the static indirect buffer
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

        // A1 × B0 draw loop. One vkCmdDrawIndexedIndirect per mesh buffer, so the
        // per-object work that A0 recorded — a bind pair and a draw call each —
        // collapses into one command per mesh. The indirect buffer is static
        // (culling off), so nothing is rebuilt here; that per-frame rebuild is
        // what experiment 2 adds.
        //
        // Batches keep the shared sorted order (material, mesh) even though B1
        // no longer needs the material grouping: the order is a fixed control, and
        // A1 needs a mesh's draws contiguous to batch them at all.
        m_pipeline.bind(handle);
        m_queries[slot].beginPipelineStats(frame.cmd); // inside rendering
        for (const MeshBatch& batch : m_batches) {
            const GpuMesh& mesh = m_meshes[batch.mesh];
            const VkBuffer vertexBuffer = mesh.vertex.handle();
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(handle, 0, 1, &vertexBuffer, &offset); // A1: still per mesh
            vkCmdBindIndexBuffer(handle, mesh.index.handle(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(handle,
                                     m_indirectBuffer.handle(),
                                     batch.firstDraw * sizeof(VkDrawIndexedIndirectCommand),
                                     batch.drawCount,
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
        r.condition = "A1xB1";
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
    // A1 keeps A0's geometry layout: one vertex/index buffer pair per mesh. Only
    // the draw technique changes, which is what makes lab_01 vs lab_03 a clean
    // A0 vs A1 pair.
    void loadModels() {
        StagingUploader up(*m_context);
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
        }
        up.flush(); // one blocking submit for all mesh copies
    }

    // Deterministic instances (dumped to JSON) + a camera loop framing them, then
    // the flattened, sorted draw list, then A1's per-mesh batches over it.
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
        // overdraw pattern. A1 additionally NEEDS this sort to be able to batch:
        // one indirect call per mesh only exists if a mesh's draws are contiguous.
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

        // Collapse the sorted list into runs of the same mesh — A1's submission
        // units. Each run becomes one vkCmdDrawIndexedIndirect.
        for (uint32_t i = 0; i < m_draws.size(); ++i) {
            if (m_batches.empty() || m_batches.back().mesh != m_draws[i].mesh) {
                m_batches.push_back(MeshBatch{m_draws[i].mesh, m_draws[i].material, i, 0});
            }
            ++m_batches.back().drawCount;
        }
        spdlog::info("scene: N={} k={} meshes={} materials={} draws={} batches={}",
                     m_instances.size(),
                     MODEL_COUNT,
                     m_meshes.size(),
                     m_materials.size(),
                     m_draws.size(),
                     m_batches.size());
    }

    // B1: ONE descriptor set (set 1) holding every material — a texture array
    // plus the materials SSBO. Nothing here is rebound per draw. Byte-identical
    // to lab_02's, so B1 does not drift between the A conditions.
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

    // A1: one indirect command per draw, index-aligned with m_draws (and hence
    // with objects[]). Culling is off in experiment 1, so this is built once and
    // costs 0 per frame — experiment 2 is where the per-frame rebuild lands.
    void createIndirect() {
        std::vector<VkDrawIndexedIndirectCommand> commands;
        commands.reserve(m_draws.size());
        for (uint32_t i = 0; i < m_draws.size(); ++i) {
            VkDrawIndexedIndirectCommand cmd{};
            cmd.indexCount = m_meshes[m_draws[i].mesh].indexCount;
            cmd.instanceCount = 1;
            cmd.firstIndex = 0;
            cmd.vertexOffset = 0;
            cmd.firstInstance = i; // -> gl_BaseInstance -> objects[i]
            commands.push_back(cmd);
        }
        const VkDeviceSize bytes = commands.size() * sizeof(VkDrawIndexedIndirectCommand);
        m_indirectBuffer =
            GpuBuffer::createDeviceLocal(*m_context, bytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        render::uploadDeviceLocal(*m_context, m_indirectBuffer, commands.data(), bytes);
    }

    // Pipeline: set 0 = frame UBO + objects, set 1 = material. No push constants
    // (the model matrix is in the objects SSBO) — log the SPIR-V hashes so the
    // shared vertex shader can be shown to be identical to lab_02's.
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
    std::vector<GpuMesh> m_meshes;
    std::vector<ModelRange> m_models; // index-aligned with MODELS
    std::vector<GpuMaterial> m_materials;
    VkSampler m_sampler{VK_NULL_HANDLE}; // raw; destroyed in ~IndirectLab
    std::vector<MaterialGpu> m_materialData;
    std::vector<ObjectGpu> m_objectData;
    GpuBuffer m_materialBuffer; // set 1, binding 0
    GpuBuffer m_objectBuffer;   // set 0, binding 1 — same place in every condition
    GpuBuffer m_indirectBuffer; // A1: static, one command per draw
    render::DescriptorSetLayout m_bindlessLayout;
    render::DescriptorSetLayout m_frameLayout;
    render::DescriptorPool m_pool;
    VkDescriptorSet m_bindlessSet{VK_NULL_HANDLE};                   // B1: exactly one
    std::array<GpuBuffer, App::FRAMES_IN_FLIGHT> m_frameUbo;         // host-visible
    std::array<VkDescriptorSet, App::FRAMES_IN_FLIGHT> m_frameSet{}; // from m_pool
    PipelineLayout m_pipelineLayout;
    GraphicsPipeline m_pipeline;

    std::vector<scene::Instance> m_instances;
    std::vector<Draw> m_draws;        // index == objects[] index == firstInstance
    std::vector<MeshBatch> m_batches; // runs of m_draws sharing a mesh
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
// only time command RECORDING. Same limitation as lab_01/lab_02; all three are
// recording-only, so the A0 vs A1 delta is still valid.
//
// NOTE — MoltenVK: Metal has no native multi-draw-indirect, so a drawCount > 1
// is emulated by encoding the commands in a loop. A1's submission saving may
// therefore not appear on Apple hardware even though the API-level call count
// dropped. Record the platform with any A0 vs A1 number.

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

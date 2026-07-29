// lab_01 — GPU-driven indirect rendering study. Baseline A0 × B0.
//
//   A0 = one VkBuffer pair (vertex+index) per mesh, plain vkCmdDrawIndexed loop.
//   B0 = one descriptor set per material, vkCmdBindDescriptorSets on change.
//
// The harness lives in template (lab::asset / scene / bench / render). This file
// only WIRES it into the A0 × B0 strategy. Design + data flow: ../README.md and
// ../indirect-rendering-experiment.md.
//

#include <exception>

#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>

#include "asset/loader.h"
#include "asset/texture.h"
#include "bench/instrument.h"
#include "bench/reporter.h"
#include "core/app.h"
#include "render/buffer.h"
#include "render/command.h"
#include "render/context.h"
#include "render/descriptor.h"
#include "render/pipeline.h"
#include "render/shader.h"
#include "scene/camera.h"
#include "scene/scene.h"

using lab::core::App;
using lab::core::FrameContext;
using lab::render::DeviceFeatures;

// STEP 1 — data types you own here (not in template, they encode the strategy):
//
//   struct GpuMesh {              // A0: no shared offsets — one buffer pair each
//       render::GpuBuffer vertex;
//       render::GpuBuffer index;
//       uint32_t indexCount;
//       int material;             // index into materials, or -1
//   };
//   struct GpuMaterial {          // B0: one descriptor set per material
//       render::GpuImage baseColor;   // + metallicRoughness, normal as needed
//       VkDescriptorSet set;          // allocated from the pool, written once
//   };
//   struct PushConstants { glm::mat4 model; };   // per-object; view/proj via UBO

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
        // Order matters: layouts before pipeline, geometry+materials before draw.
        //
        // STEP 2 — LOAD MODELS (set M)
        //   for each model path: asset::loadModel(path) -> CpuModel.
        //   Keep the CPU data only as long as you need it for upload.
        //
        // STEP 3 — A0 GPU UPLOAD
        //   StagingUploader up(*m_context);
        //   per mesh: create two device-local buffers, queue both copies:
        //     GpuBuffer::createDeviceLocal(*m_context, vbytes, VERTEX_BUFFER_BIT)
        //     GpuBuffer::createDeviceLocal(*m_context, ibytes, INDEX_BUFFER_BIT)
        //     up.upload(vbuf, verts.data(), vbytes);
        //     up.upload(ibuf, idx.data(),   ibytes);
        //   up.flush();  // one blocking submit for the whole set
        //
        // STEP 4 — B0 MATERIALS
        //   Build ONE DescriptorSetLayout (combined image sampler(s) [+ UBO]).
        //   Create a VkSampler here (mip/anisotropy = a fixed control — lab owns).
        //   DescriptorPool sized for material count.
        //   per material: asset::loadImage2D(*m_context, path, srgb) -> GpuImage
        //     (srgb=true for baseColor, false for normal/metallicRoughness).
        //     Model paths are the glTF's .png/.jpg — stb loads them directly, no
        //     KTX/BC7 step. (loadKtx stays for IBL cubemaps only.)
        //     set = pool.allocate(layout.handle());
        //     DescriptorWriter(*m_context).writeImage(set, binding, ...).flush();
        //
        // STEP 5 — PIPELINE
        //   Shader vert/frag = Shader::fromFile(*m_context, "shaders/....spv");
        //   PipelineLayout: the material set layout + a PushConstantRange(model).
        //   auto vin = asset::standardVertexInput();  // matches asset::Vertex
        //   PipelineBuilder(*m_context)
        //       .shaders(vert.handle(), frag.handle())
        //       .layout(layout)
        //       .vertexInput(vin.bindings, vin.attributes)
        //       .build();          // fixed render state already baked
        //
        // STEP 6 — SCENE (deterministic)
        //   scene::SceneParams p{.seed=42, .count=N, .modelCount=k};
        //   auto instances = scene::generateScene(p);
        //   scene::dumpSceneJson(instances, p, "scene.json");   // audit artifact
        //   glm::vec3 c = scene::sceneCenter(instances);
        //   m_path = scene::makeSweepPath(c, radius, height);   // camera loop
        //
        // STEP 7 — BENCH
        //   m_queries.emplace(*m_context);           // GpuQueries (needs Context&)
        //   bench::dumpEnvironmentJson(*m_context, "env.json");
        //   CsvReporter over "results.csv".
    }

    ~IndirectLab() override {
        // GPU may still be using your buffers/pipeline; drain before members die.
        vkDeviceWaitIdle(m_context->getDevice());
    }

protected:
    void onRender(const FrameContext& frame) override {
        VkCommandBuffer handle = frame.cmd.getHandle();

        // STEP 8 — FRAME INDEX / WARM-UP
        //   Track a frame counter. Discard the first 300 frames (and 300 after any
        //   condition switch) before recording measurements. m_frame % 720 feeds
        //   the camera loop.

        // STEP 9 — CPU RECORD TIMER
        //   m_recordTimer.start();   // stop just before vkCmdEndRendering below.
        //   (Submit span is measured by App::drawFrame, not here — see NOTE.)

        // STEP 10 — GPU QUERIES: reset + begin (before begin-rendering)
        //   m_queries->reset(frame.cmd);
        //   m_queries->writeBeginTimestamp(frame.cmd);

        // Color + depth must be attachment-optimal before rendering. Depth is
        // discarded each frame (UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL).
        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // STEP 11 — add the depth transition (frame.depth, aspect = DEPTH_BIT) so
        // the baked depth test has a target:
        //   frame.cmd.transitionImageLayout(frame.depth, UNDEFINED,
        //       DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

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

        // STEP 12 — build a VkRenderingAttachmentInfo for depth (frame.depthView,
        // loadOp CLEAR depth=1.0, storeOp DONT_CARE) and set rendering.pDepthAttachment.

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea.extent = frame.extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colorAttachment;

        vkCmdBeginRendering(handle, &rendering);

        // STEP 13 — DYNAMIC VIEWPORT/SCISSOR (PipelineBuilder bakes them dynamic)
        //   VkViewport vp{0,0,(float)w,(float)h,0,1}; vkCmdSetViewport(handle,0,1,&vp);
        //   VkRect2D sc{{0,0}, frame.extent};        vkCmdSetScissor(handle,0,1,&sc);

        // STEP 14 — CAMERA
        //   float aspect = (float)frame.extent.width / frame.extent.height;
        //   auto cam = scene::sampleCamera(m_path, m_frame, aspect);
        //   glm::mat4 projView = cam.proj * cam.view;   // upload via UBO or PC.

        // STEP 15 — PIPELINE STATS bracket + DRAW LOOP
        //   m_queries->beginPipelineStats(frame.cmd);
        //   m_pipeline.bind(handle);
        //   Iterate instances in a FIXED order (sort by material, then mesh, once
        //   at setup — identical across conditions):
        //     if (material changed)
        //         vkCmdBindDescriptorSets(handle, GRAPHICS, m_pipeline.layout(),
        //                                 0, 1, &materials[m].set, 0, nullptr);  // B0
        //     VkBuffer vb = mesh.vertex.handle(); VkDeviceSize off = 0;
        //     vkCmdBindVertexBuffers(handle, 0, 1, &vb, &off);                    // A0
        //     vkCmdBindIndexBuffer(handle, mesh.index.handle(), 0, UINT32);
        //     PushConstants pc{ instance.model };
        //     vkCmdPushConstants(handle, m_pipeline.layout(), VERTEX_BIT, 0,
        //                        sizeof(pc), &pc);
        //     vkCmdDrawIndexed(handle, mesh.indexCount, 1, 0, 0, 0);
        //   m_queries->endPipelineStats(frame.cmd);

        // STEP 16 — m_recordTimer.stop... (record span ends here, before End).

        vkCmdEndRendering(handle);

        // STEP 17 — m_queries->writeEndTimestamp(frame.cmd);  // after end-rendering

        frame.cmd.transitionImageLayout(frame.image,
                                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        // STEP 18 — RESOLVE + REPORT (only for non-warm-up frames)
        //   Resolve blocks on the queries, so do it AFTER this frame's fence is
        //   signalled — i.e. next frame, or in onSwapchainRecreated-style hook.
        //   Simplest: keep the previous frame's GpuQueries and resolve it now:
        //     double gpuMs = m_queries->resolveGpuMilliseconds();
        //     auto stats   = m_queries->resolvePipelineStats();
        //     bench::Record r{...condition, w, h, N, rep, gpuMs, recordMs, ...};
        //     m_reporter->write(r);
        //
        // ++m_frame;
    }

private:
    // STEP 19 — MEMBERS (uncomment/add as you implement; destruction order =
    // reverse declaration, so declare Context-derived resources after nothing
    // that outlives them):
    //   std::vector<GpuMesh> m_meshes;
    //   std::vector<GpuMaterial> m_materials;
    //   VkSampler m_sampler{VK_NULL_HANDLE};      // destroy in ~IndirectLab
    //   render::DescriptorSetLayout m_materialLayout;
    //   render::DescriptorPool m_pool;
    //   render::PipelineLayout m_pipelineLayout;
    //   render::GraphicsPipeline m_pipeline;
    //   scene::CameraPath m_path;
    //   std::optional<bench::GpuQueries> m_queries;   // no default ctor
    //   std::optional<bench::CsvReporter> m_reporter;
    //   bench::CpuTimer m_recordTimer;
    //   uint64_t m_frame{0};
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

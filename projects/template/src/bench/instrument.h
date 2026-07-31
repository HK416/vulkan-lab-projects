#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace lab::render {
class Context;
class CommandBuffer;
} // namespace lab::render

namespace lab::bench {

// GPU timestamps + pipeline statistics for one frame. Record into the command
// buffer during rendering, then resolve after the frame's fence is signalled.
// On devices without timestamp support (some MoltenVK paths) GPU time resolves
// to 0 and is flagged.
class GpuQueries {
public:
    // Recorded pipeline statistics (field order matches the query pool flags).
    struct PipelineStats {
        uint64_t inputAssemblyVertices = 0;
        uint64_t inputAssemblyPrimitives = 0; // triangle count for a triangle list
        uint64_t vertexShaderInvocations = 0;
        uint64_t clippingPrimitives = 0;
        uint64_t fragmentShaderInvocations = 0;
    };

    GpuQueries() = delete;
    GpuQueries(const GpuQueries&) = delete;
    GpuQueries& operator=(const GpuQueries&) = delete;
    // Movable so a lab can keep one per frame-in-flight in a plain vector. The
    // alternative — an array of std::optional — is a construct no static analysis
    // can prove is engaged, so every access reads as a possible null deref.
    GpuQueries(GpuQueries&& other) noexcept;
    GpuQueries& operator=(GpuQueries&& other) noexcept;
    explicit GpuQueries(render::Context& context);
    ~GpuQueries();

    // Call once at the start of recording, before begin-rendering.
    void reset(render::CommandBuffer& cmd);

    // Bracket the GPU work whose duration you want (top/bottom of pipe).
    void writeBeginTimestamp(render::CommandBuffer& cmd);
    void writeEndTimestamp(render::CommandBuffer& cmd);

    // Bracket the draws whose pipeline statistics you want.
    void beginPipelineStats(render::CommandBuffer& cmd);
    void endPipelineStats(render::CommandBuffer& cmd);

    // Resolve the recorded values. Blocks until available, so call only after the
    // frame's fence has signalled. gpuSupported() reports timestamp validity.
    double resolveGpuMilliseconds();
    PipelineStats resolvePipelineStats();
    bool gpuSupported() const {
        return m_timestampsSupported;
    }

private:
    void destroy() noexcept;

    render::Context* m_context;
    VkQueryPool m_timestampPool{VK_NULL_HANDLE};
    VkQueryPool m_statsPool{VK_NULL_HANDLE};
    float m_timestampPeriod{1.0f}; // ns per tick, from device limits
    bool m_timestampsSupported{true};
};

// Monotonic stopwatch for CPU spans (command recording, submit). Wraps a steady
// clock; kept out of the header to avoid pulling <chrono> everywhere.
class CpuTimer {
public:
    void start();
    double stopMilliseconds() const; // elapsed since the last start()

private:
    uint64_t m_startNs = 0;
};

} // namespace lab::bench

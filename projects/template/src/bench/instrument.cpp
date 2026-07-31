#include "bench/instrument.h"

#include <chrono>

#include "core/vk_check.h"
#include "render/command.h"
#include "render/context.h"

namespace lab::bench {

// --- GpuQueries -----------------------------------------------------------

namespace {
// Bit order of a VkQueryPool result equals ascending flag-bit value. These five
// flags therefore resolve in exactly PipelineStats field order.
constexpr VkQueryPipelineStatisticFlags STATS_FLAGS =
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
constexpr uint32_t TIMESTAMP_COUNT = 2; // begin + end
} // namespace

GpuQueries::GpuQueries(render::Context& context) : m_context(&context) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(context.getPhysicalDevice(), &props);
    m_timestampPeriod = props.limits.timestampPeriod;
    // timestampPeriod == 0 or no compute/graphics timestamps => unsupported
    // (some MoltenVK paths). GPU time then resolves to 0.
    m_timestampsSupported = props.limits.timestampComputeAndGraphics != VK_FALSE &&
                            props.limits.timestampPeriod != 0.0f;

    if (m_timestampsSupported) {
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = TIMESTAMP_COUNT;
        VK_CHECK(vkCreateQueryPool(context.getDevice(), &info, nullptr, &m_timestampPool));
    }

    // Pipeline statistics need the feature enabled at device creation (Context
    // enables it when supported). If absent, leave m_statsPool null: the stats
    // path no-ops and resolves to zeros.
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(context.getPhysicalDevice(), &features);
    if (features.pipelineStatisticsQuery) {
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info.queryCount = 1;
        info.pipelineStatistics = STATS_FLAGS;
        VK_CHECK(vkCreateQueryPool(context.getDevice(), &info, nullptr, &m_statsPool));
    }
}

GpuQueries::GpuQueries(GpuQueries&& other) noexcept
    : m_context(other.m_context), m_timestampPool(other.m_timestampPool),
      m_statsPool(other.m_statsPool), m_timestampPeriod(other.m_timestampPeriod),
      m_timestampsSupported(other.m_timestampsSupported) {
    other.m_timestampPool = VK_NULL_HANDLE;
    other.m_statsPool = VK_NULL_HANDLE;
}

GpuQueries& GpuQueries::operator=(GpuQueries&& other) noexcept {
    if (this != &other) {
        destroy();
        m_context = other.m_context;
        m_timestampPool = other.m_timestampPool;
        m_statsPool = other.m_statsPool;
        m_timestampPeriod = other.m_timestampPeriod;
        m_timestampsSupported = other.m_timestampsSupported;
        other.m_timestampPool = VK_NULL_HANDLE;
        other.m_statsPool = VK_NULL_HANDLE;
    }
    return *this;
}

GpuQueries::~GpuQueries() {
    destroy();
}

void GpuQueries::destroy() noexcept {
    if (m_timestampPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_context->getDevice(), m_timestampPool, nullptr);
        m_timestampPool = VK_NULL_HANDLE;
    }
    if (m_statsPool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(m_context->getDevice(), m_statsPool, nullptr);
        m_statsPool = VK_NULL_HANDLE;
    }
}

void GpuQueries::reset(render::CommandBuffer& cmd) {
    // Pools must be reset before reuse each frame.
    if (m_timestampPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd.getHandle(), m_timestampPool, 0, TIMESTAMP_COUNT);
    }
    if (m_statsPool != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(cmd.getHandle(), m_statsPool, 0, 1);
    }
}

void GpuQueries::writeBeginTimestamp(render::CommandBuffer& cmd) {
    if (m_timestampPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(cmd.getHandle(),
                             VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                             m_timestampPool,
                             0);
    }
}

void GpuQueries::writeEndTimestamp(render::CommandBuffer& cmd) {
    if (m_timestampPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(cmd.getHandle(),
                             VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             m_timestampPool,
                             1);
    }
}

void GpuQueries::beginPipelineStats(render::CommandBuffer& cmd) {
    if (m_statsPool != VK_NULL_HANDLE) {
        vkCmdBeginQuery(cmd.getHandle(), m_statsPool, 0, 0);
    }
}

void GpuQueries::endPipelineStats(render::CommandBuffer& cmd) {
    if (m_statsPool != VK_NULL_HANDLE) {
        vkCmdEndQuery(cmd.getHandle(), m_statsPool, 0);
    }
}

double GpuQueries::resolveGpuMilliseconds() {
    if (m_timestampPool == VK_NULL_HANDLE) {
        return 0.0;
    }
    uint64_t ticks[TIMESTAMP_COUNT] = {0, 0};
    VK_CHECK(vkGetQueryPoolResults(m_context->getDevice(),
                                   m_timestampPool,
                                   0,
                                   TIMESTAMP_COUNT,
                                   sizeof(ticks),
                                   ticks,
                                   sizeof(uint64_t),
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    const uint64_t delta = ticks[1] - ticks[0]; // wraps correctly in unsigned
    return static_cast<double>(delta) * static_cast<double>(m_timestampPeriod) / 1.0e6;
}

GpuQueries::PipelineStats GpuQueries::resolvePipelineStats() {
    PipelineStats stats;
    if (m_statsPool == VK_NULL_HANDLE) {
        return stats;
    }
    uint64_t values[5] = {0, 0, 0, 0, 0};
    VK_CHECK(vkGetQueryPoolResults(m_context->getDevice(),
                                   m_statsPool,
                                   0,
                                   1,
                                   sizeof(values),
                                   values,
                                   sizeof(values),
                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    stats.inputAssemblyVertices = values[0];
    stats.inputAssemblyPrimitives = values[1];
    stats.vertexShaderInvocations = values[2];
    stats.clippingPrimitives = values[3];
    stats.fragmentShaderInvocations = values[4];
    return stats;
}

// --- CpuTimer -------------------------------------------------------------

namespace {
uint64_t steadyNowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
} // namespace

void CpuTimer::start() {
    m_startNs = steadyNowNs();
}

double CpuTimer::stopMilliseconds() const {
    return static_cast<double>(steadyNowNs() - m_startNs) / 1.0e6;
}

} // namespace lab::bench

#pragma once

#include <cstdint>
#include <string>

namespace lab::render {
class Context;
}

namespace lab::bench {

// One measured data point: the condition it was taken under plus the metrics.
// The caller fills metrics with per-run medians (see the reliability notes in the
// experiment design).
struct Record {
    // Condition
    std::string condition; // e.g. "A0xB0"
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t objectCount = 0; // N
    uint32_t repetition = 0;  // repeat index

    // Metrics
    double gpuMs = 0.0;
    double cpuRecordMs = 0.0;
    double cpuSubmitMs = 0.0;
    uint64_t triangles = 0;
    uint64_t vertexInvocations = 0;
    uint64_t fragmentInvocations = 0;
    uint64_t deviceMemoryBytes = 0;
    uint32_t deviceAllocations = 0;
};

// Appends rows to a CSV, writing the header row when the file is first created.
class CsvReporter {
public:
    explicit CsvReporter(std::string path);
    void write(const Record& record);

private:
    std::string m_path;
    bool m_headerWritten = false;
};

// Captures fixed environment metadata (GPU, driver version, API version, OS)
// once, to JSON — the "must record" list from the experiment design.
void dumpEnvironmentJson(render::Context& context, const std::string& path);

} // namespace lab::bench

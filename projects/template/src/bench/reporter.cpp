#include "bench/reporter.h"

#include <fstream>
#include <stdexcept>

#include <vulkan/vulkan.h>

#include "render/context.h"

namespace lab::bench {

// --- CsvReporter ----------------------------------------------------------

CsvReporter::CsvReporter(std::string path) : m_path(std::move(path)) {
    // Header is written lazily on the first write(). If the file already has
    // content (a resumed run appending to an existing CSV), keep its header.
    std::ifstream existing(m_path, std::ios::ate | std::ios::binary);
    m_headerWritten = existing && existing.tellg() > 0;
}

void CsvReporter::write(const Record& record) {
    std::ofstream out(m_path, std::ios::app);
    if (!out) {
        throw std::runtime_error("CsvReporter: cannot open " + m_path);
    }

    if (!m_headerWritten) {
        out << "condition,width,height,objectCount,repetition,"
               "gpuMs,cpuRecordMs,cpuSubmitMs,triangles,vertexInvocations,"
               "fragmentInvocations,deviceMemoryBytes,deviceAllocations\n";
        m_headerWritten = true;
    }

    out << record.condition << ',' << record.width << ',' << record.height << ','
        << record.objectCount << ',' << record.repetition << ',' << record.gpuMs << ','
        << record.cpuRecordMs << ',' << record.cpuSubmitMs << ',' << record.triangles << ','
        << record.vertexInvocations << ',' << record.fragmentInvocations << ','
        << record.deviceMemoryBytes << ',' << record.deviceAllocations << '\n';
}

// --- environment ----------------------------------------------------------

namespace {
const char* osName() {
#if defined(__APPLE__)
    return "macOS";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}
} // namespace

void dumpEnvironmentJson(render::Context& context, const std::string& path) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(context.getPhysicalDevice(), &props);

    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("dumpEnvironmentJson: cannot open " + path);
    }

    out << "{\n";
    out << "  \"gpu\": \"" << props.deviceName << "\",\n";
    out << "  \"vendorId\": " << props.vendorID << ",\n";
    out << "  \"deviceId\": " << props.deviceID << ",\n";
    out << "  \"driverVersion\": " << props.driverVersion << ",\n";
    out << "  \"apiVersion\": \"" << VK_API_VERSION_MAJOR(props.apiVersion) << '.'
        << VK_API_VERSION_MINOR(props.apiVersion) << '.' << VK_API_VERSION_PATCH(props.apiVersion)
        << "\",\n";
    out << "  \"os\": \"" << osName() << "\"\n";
    out << "}\n";
}

} // namespace lab::bench

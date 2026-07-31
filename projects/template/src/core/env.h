#pragma once

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

namespace lab::core {

// Run configuration comes from the environment, not argv: a sweep script drives
// every lab the same way and no lab's main() has to grow an argument parser.
// See App (LAB_FRAMES / LAB_CAPTURE / LAB_CAPTURE_FILE) and each lab's README
// for the variables actually consumed.

// Absent or unparseable = fallback. A typo must not silently change a benchmark
// condition, so anything that is not a plain number is reported.
inline int64_t envInt(const char* name, int64_t fallback) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return fallback;
    }
    try {
        return std::stoll(raw);
    } catch (const std::exception&) {
        spdlog::warn("{}='{}' is not a number; ignoring", name, raw);
        return fallback;
    }
}

inline std::string envStr(const char* name, const std::string& fallback) {
    const char* raw = std::getenv(name);
    return (raw != nullptr && *raw != '\0') ? std::string(raw) : fallback;
}

} // namespace lab::core

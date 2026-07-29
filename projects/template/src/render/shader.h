#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace lab::render {

class Context;

// RAII VkShaderModule loaded from SPIR-V, plus a stable hash of the SPIR-V bytes.
// The hash serves the experiment's "record the SPIR-V hash" requirement: it lets
// you verify that conditions meant to share a shader really compiled the same
// bytes (branching only via #define, not diverging). 64-bit FNV-1a — an identity
// fingerprint for logging, not a cryptographic digest.
class Shader {
public:
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    Shader() = default;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;
    ~Shader();

    static Shader fromSpirv(Context& context, const uint32_t* code, size_t sizeBytes);
    static Shader fromFile(Context& context, const std::string& path);

    VkShaderModule handle() const {
        return m_module;
    }

    uint64_t hash() const {
        return m_hash;
    }

    std::string hashHex() const; // 16-char hex of hash(), for the report

private:
    void destroy() noexcept;

    Context* m_context{nullptr};
    VkShaderModule m_module{VK_NULL_HANDLE};
    uint64_t m_hash = 0;
};

} // namespace lab::render

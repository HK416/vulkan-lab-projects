#include "render/shader.h"

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "core/vk_check.h"
#include "render/context.h"

namespace lab::render {
namespace {

// 64-bit FNV-1a over the raw SPIR-V bytes. Identity fingerprint for the report,
// not a cryptographic digest (see header).
uint64_t fnv1a(const uint8_t* data, size_t size) {
    uint64_t hash = 14695981039346656037ull; // offset basis
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull; // FNV prime
    }
    return hash;
}

} // namespace

Shader Shader::fromSpirv(Context& context, const uint32_t* code, size_t sizeBytes) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = sizeBytes; // bytes, not words
    info.pCode = code;

    Shader out;
    out.m_context = &context;
    VK_CHECK(vkCreateShaderModule(context.getDevice(), &info, nullptr, &out.m_module));
    out.m_hash = fnv1a(reinterpret_cast<const uint8_t*>(code), sizeBytes);
    return out;
}

Shader Shader::fromFile(Context& context, const std::string& path) {
    // ate: open at end so tellg() gives the size in one shot.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Shader::fromFile: cannot open " + path);
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0) {
        throw std::runtime_error("Shader::fromFile: not 4-byte-aligned SPIR-V: " + path);
    }

    std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(code.data()), size)) {
        throw std::runtime_error("Shader::fromFile: read failed: " + path);
    }
    return fromSpirv(context, code.data(), static_cast<size_t>(size));
}

std::string Shader::hashHex() const {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(m_hash));
    return std::string(buf);
}

Shader::Shader(Shader&& other) noexcept
    : m_context(other.m_context), m_module(other.m_module), m_hash(other.m_hash) {
    other.m_module = VK_NULL_HANDLE;
    other.m_hash = 0;
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        destroy();
        m_context = other.m_context;
        m_module = other.m_module;
        m_hash = other.m_hash;
        other.m_module = VK_NULL_HANDLE;
        other.m_hash = 0;
    }
    return *this;
}

Shader::~Shader() {
    destroy();
}

void Shader::destroy() noexcept {
    if (m_module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_context->getDevice(), m_module, nullptr);
        m_module = VK_NULL_HANDLE;
    }
}

} // namespace lab::render

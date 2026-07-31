#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace lab::asset {

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec4 tangent; // xyz + w handedness sign (glTF convention), for normal mapping
    glm::vec2 uv;
};

// Vertex input for the interleaved Vertex layout above — the fixed AoS layout
// every condition uses. Defined once here (not per lab) so the control can't
// drift; pass to render::PipelineBuilder::vertexInput(). Kept in the asset layer
// because render must not depend on asset.
struct VertexInputDesc {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
};

VertexInputDesc standardVertexInput();

// One primitive's CPU geometry plus the material it uses. A model in the set M
// is a list of these.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    int material = -1; // index into CpuModel::materials, or -1 for none
};

// Metallic-roughness PBR inputs. Textures are resolved image paths (loaded as
// loaded as RGBA8 via asset::loadImage2D); empty string means "use the factor alone".
struct MaterialData {
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    std::string baseColorTexture;
    std::string metallicRoughnessTexture;
    std::string normalTexture;
};

// One loaded glTF file: primitives and materials, all still on the CPU. Each lab
// uploads these to the GPU with its own buffer/binding strategy (A0 multi-buffer,
// A2/A3 single buffer, ...), which is why the loader stays strategy-neutral.
struct CpuModel {
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;

    // AABB over every mesh, in the space the vertices are uploaded in. The model
    // set M mixes wildly different authored scales (Avocado ~0.07 units, Lantern
    // ~5), so a scene that placed them raw would give each model a different
    // fragment load — and the resolution axis would stop meaning anything. Labs
    // normalize placement against this; it is measured, not guessed per model.
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

// Loads a .gltf/.glb via cgltf. Throws std::runtime_error on failure. Reads
// position/normal/tangent/uv and metallic-roughness materials (no skinning,
// morph targets, or emissive).
CpuModel loadModel(const std::string& path);

} // namespace lab::asset

#include "asset/loader.h"

#include <cstddef>
#include <stdexcept>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace lab::asset {

VertexInputDesc standardVertexInput() {
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, tangent)},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
    };
    return {{binding}, attributes};
}

namespace {

// Directory prefix of a file path (with trailing slash), for resolving relative
// image URIs against the glTF's location.
std::string dirOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash + 1);
}

// Resolved on-disk path of a texture view's image, or "" when absent. Assumes an
// external URI (no embedded/buffer-view images) and no percent-encoding — the
// lab's assets are plain files.
// ponytail: no cgltf_decode_uri; add if any asset URI is percent-encoded.
std::string texturePath(const cgltf_texture_view& view, const std::string& dir) {
    if (!view.texture || !view.texture->image || !view.texture->image->uri) {
        return {};
    }
    return dir + view.texture->image->uri;
}

} // namespace

CpuModel loadModel(const std::string& path) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        throw std::runtime_error("loadModel: parse failed: " + path);
    }
    struct Guard {
        cgltf_data* d;
        ~Guard() {
            cgltf_free(d);
        }
    } guard{data};

    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        throw std::runtime_error("loadModel: buffer load failed: " + path);
    }
    if (cgltf_validate(data) != cgltf_result_success) {
        throw std::runtime_error("loadModel: validation failed: " + path);
    }

    const std::string dir = dirOf(path);
    CpuModel model;

    model.materials.reserve(data->materials_count);
    for (size_t i = 0; i < data->materials_count; ++i) {
        const cgltf_material& src = data->materials[i];
        MaterialData mat;
        if (src.has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& pbr = src.pbr_metallic_roughness;
            mat.baseColorFactor = {pbr.base_color_factor[0],
                                   pbr.base_color_factor[1],
                                   pbr.base_color_factor[2],
                                   pbr.base_color_factor[3]};
            mat.metallicFactor = pbr.metallic_factor;
            mat.roughnessFactor = pbr.roughness_factor;
            mat.baseColorTexture = texturePath(pbr.base_color_texture, dir);
            mat.metallicRoughnessTexture = texturePath(pbr.metallic_roughness_texture, dir);
        }
        mat.normalTexture = texturePath(src.normal_texture, dir);
        model.materials.push_back(std::move(mat));
    }

    for (size_t m = 0; m < data->meshes_count; ++m) {
        const cgltf_mesh& mesh = data->meshes[m];
        for (size_t p = 0; p < mesh.primitives_count; ++p) {
            const cgltf_primitive& prim = mesh.primitives[p];

            // POSITION is required and fixes the vertex count.
            const cgltf_accessor* positions = nullptr;
            for (size_t a = 0; a < prim.attributes_count; ++a) {
                if (prim.attributes[a].type == cgltf_attribute_type_position) {
                    positions = prim.attributes[a].data;
                }
            }
            if (!positions) {
                continue; // skip a primitive with no positions
            }

            MeshData out;
            out.vertices.resize(positions->count);
            for (size_t a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                for (size_t v = 0; v < out.vertices.size(); ++v) {
                    Vertex& vert = out.vertices[v];
                    switch (attr.type) {
                    case cgltf_attribute_type_position:
                        cgltf_accessor_read_float(attr.data, v, &vert.position.x, 3);
                        break;
                    case cgltf_attribute_type_normal:
                        cgltf_accessor_read_float(attr.data, v, &vert.normal.x, 3);
                        break;
                    case cgltf_attribute_type_tangent:
                        cgltf_accessor_read_float(attr.data, v, &vert.tangent.x, 4);
                        break;
                    case cgltf_attribute_type_texcoord:
                        if (attr.index == 0) {
                            cgltf_accessor_read_float(attr.data, v, &vert.uv.x, 2);
                        }
                        break;
                    default:
                        break; // color/joints/weights not used
                    }
                }
            }

            if (prim.indices) {
                out.indices.resize(prim.indices->count);
                for (size_t i = 0; i < out.indices.size(); ++i) {
                    out.indices[i] =
                        static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
                }
            } else {
                // Non-indexed geometry: synthesize a sequential index buffer so
                // every lab can assume indexed draws.
                out.indices.resize(out.vertices.size());
                for (uint32_t i = 0; i < out.indices.size(); ++i) {
                    out.indices[i] = i;
                }
            }

            out.material = prim.material ? static_cast<int>(prim.material - data->materials) : -1;
            model.meshes.push_back(std::move(out));
        }
    }

    // AABB over everything that was actually loaded (one pass; positions are
    // already in memory). Left at 0 for an empty model.
    bool first = true;
    for (const MeshData& mesh : model.meshes) {
        for (const Vertex& v : mesh.vertices) {
            if (first) {
                model.min = model.max = v.position;
                first = false;
            } else {
                model.min = glm::min(model.min, v.position);
                model.max = glm::max(model.max, v.position);
            }
        }
    }

    return model;
}

} // namespace lab::asset

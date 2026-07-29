#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace lab::scene {

// One placed object in the fixed scene: which model from set M, where, and its
// orientation. Generated once from a seed and dumped to JSON so every render
// condition uses byte-identical placements.
struct Instance {
    uint32_t model = 0; // index into the model set M
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct SceneParams {
    uint64_t seed = 42;
    uint32_t count = 0;      // N — number of instances
    uint32_t modelCount = 1; // k = |M|; instance.model is in [0, k)
    float spacing = 2.0f;    // grid cell size on the XZ plane
    float jitter = 0.5f;     // max +/- positional jitter per axis
};

// Deterministic and platform-independent (fixed integer PRNG, not std
// distributions): the same params always yield the same instances on any
// machine, so conditions stay comparable.
std::vector<Instance> generateScene(const SceneParams& params);

// Writes the placement to JSON for cross-condition auditing (schema in README).
void dumpSceneJson(const std::vector<Instance>& instances,
                   const SceneParams& params,
                   const std::string& path);

// Centroid of all instance positions — a natural camera look-at target.
glm::vec3 sceneCenter(const std::vector<Instance>& instances);

// Half-extent (max |pos - center| per axis) — for sizing the camera sweep.
glm::vec3 sceneHalfExtent(const std::vector<Instance>& instances);

} // namespace lab::scene

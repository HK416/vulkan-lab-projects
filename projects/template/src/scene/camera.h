#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace lab::scene {

// Deterministic fly-through camera. The eye follows a closed Catmull-Rom loop
// over control points, sampled purely by frame index — no wall-clock, no
// accumulated dt — so a given frame always yields the exact same view. One loop
// is loopFrames frames long (720 for the experiment).
struct CameraPath {
    std::vector<glm::vec3> controlPoints; // >= 4, treated as a closed loop
    glm::vec3 target{0.0f};               // constant look-at point (scene center)
    float fovYRadians = glm::radians(60.0f);
    float nearZ = 0.1f;
    float farZ = 500.0f;
    uint32_t loopFrames = 720;
};

struct CameraSample {
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f}; // Vulkan clip space: depth 0..1, Y flipped
    glm::vec3 position{0.0f};
};

// aspect = width / height. frameIndex is taken modulo loopFrames.
CameraSample sampleCamera(const CameraPath& path, uint64_t frameIndex, float aspect);

// Builds a closed loop that sweeps a horizontal field: `points` control points
// on a circle of radius `radius` at height `center.y + height`, all looking at
// `center`. Tune radius/height so the in-frustum object ratio stays uniform.
CameraPath makeSweepPath(const glm::vec3& center,
                         float radius,
                         float height,
                         uint32_t points = 8,
                         uint32_t loopFrames = 720);

} // namespace lab::scene

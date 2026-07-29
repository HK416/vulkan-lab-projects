#include "scene/camera.h"

#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace lab::scene {
namespace {

// Uniform Catmull-Rom (tension 0.5) through p1..p2, with p0/p3 as neighbours.
glm::vec3 catmullRom(const glm::vec3& p0,
                     const glm::vec3& p1,
                     const glm::vec3& p2,
                     const glm::vec3& p3,
                     float u) {
    const float u2 = u * u;
    const float u3 = u2 * u;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * u + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * u3);
}

} // namespace

CameraSample sampleCamera(const CameraPath& path, uint64_t frameIndex, float aspect) {
    CameraSample sample;

    const size_t n = path.controlPoints.size();
    if (n == 0) {
        return sample; // nothing to sample; identity view/proj
    }

    // Position purely from frame index: a full loop spans loopFrames frames and
    // n path segments (closed), so no wall-clock and no accumulated error.
    const uint32_t loopFrames = path.loopFrames == 0 ? 1 : path.loopFrames;
    const double loopT = static_cast<double>(frameIndex % loopFrames) / loopFrames;
    const double scaled = loopT * n;
    const size_t seg = static_cast<size_t>(scaled) % n;
    const float u = static_cast<float>(scaled - std::floor(scaled));

    const glm::vec3& p0 = path.controlPoints[(seg + n - 1) % n];
    const glm::vec3& p1 = path.controlPoints[seg];
    const glm::vec3& p2 = path.controlPoints[(seg + 1) % n];
    const glm::vec3& p3 = path.controlPoints[(seg + 2) % n];

    const glm::vec3 eye = catmullRom(p0, p1, p2, p3, u);
    sample.position = eye;
    sample.view = glm::lookAt(eye, path.target, glm::vec3(0.0f, 1.0f, 0.0f));

    // GLM_FORCE_DEPTH_ZERO_TO_ONE is set workspace-wide, so depth is already
    // Vulkan's 0..1. Flip Y to match Vulkan's clip-space orientation.
    sample.proj =
        glm::perspective(path.fovYRadians, aspect <= 0.0f ? 1.0f : aspect, path.nearZ, path.farZ);
    sample.proj[1][1] *= -1.0f;
    return sample;
}

CameraPath makeSweepPath(const glm::vec3& center,
                         float radius,
                         float height,
                         uint32_t points,
                         uint32_t loopFrames) {
    CameraPath path;
    path.target = center;
    path.loopFrames = loopFrames;

    const uint32_t count = points < 4 ? 4 : points; // Catmull-Rom needs >= 4
    path.controlPoints.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const float angle = glm::two_pi<float>() * static_cast<float>(i) / count;
        path.controlPoints.push_back({center.x + radius * std::cos(angle),
                                      center.y + height,
                                      center.z + radius * std::sin(angle)});
    }
    return path;
}

} // namespace lab::scene

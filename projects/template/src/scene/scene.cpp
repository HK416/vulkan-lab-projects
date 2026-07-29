#include "scene/scene.h"

#include <cmath>
#include <fstream>
#include <stdexcept>

#include <glm/gtc/constants.hpp>

namespace lab::scene {
namespace {

// splitmix64: a fixed integer PRNG. No std distributions (implementation-defined
// across libc++/libstdc++), so the same seed yields identical scenes everywhere.
struct Rng {
    uint64_t state;
    uint64_t next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    // [0, 1)
    float unit() {
        return static_cast<float>((next() >> 11) * (1.0 / 9007199254740992.0));
    }
    // [-a, a]
    float signedUnit(float a) {
        return (2.0f * unit() - 1.0f) * a;
    }
};

} // namespace

std::vector<Instance> generateScene(const SceneParams& params) {
    std::vector<Instance> instances;
    instances.reserve(params.count);

    Rng rng{params.seed};
    const uint32_t modelCount = params.modelCount == 0 ? 1 : params.modelCount;

    // Near-square grid on XZ, centered on the origin, one instance per cell.
    const uint32_t cols =
        params.count == 0
            ? 0
            : static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(params.count))));
    const uint32_t rows = cols == 0 ? 0 : (params.count + cols - 1) / cols;

    for (uint32_t i = 0; i < params.count; ++i) {
        const uint32_t c = i % cols;
        const uint32_t r = i / cols;

        Instance inst;
        inst.model = static_cast<uint32_t>(rng.next() % modelCount);
        inst.position = {(c - (cols - 1) * 0.5f) * params.spacing + rng.signedUnit(params.jitter),
                         0.0f,
                         (r - (rows - 1) * 0.5f) * params.spacing + rng.signedUnit(params.jitter)};

        // Uniform random orientation (Shoemake): even coverage of SO(3).
        const float u1 = rng.unit();
        const float u2 = rng.unit();
        const float u3 = rng.unit();
        const float s1 = std::sqrt(1.0f - u1);
        const float s2 = std::sqrt(u1);
        const float t2 = glm::two_pi<float>() * u2;
        const float t3 = glm::two_pi<float>() * u3;
        inst.rotation = glm::quat(/*w=*/s2 * std::cos(t3),
                                  /*x=*/s1 * std::sin(t2),
                                  /*y=*/s1 * std::cos(t2),
                                  /*z=*/s2 * std::sin(t3));

        instances.push_back(inst);
    }
    return instances;
}

void dumpSceneJson(const std::vector<Instance>& instances,
                   const SceneParams& params,
                   const std::string& path) {
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("dumpSceneJson: cannot open " + path);
    }
    out.precision(9); // enough for round-tripping float32

    out << "{\n";
    out << "  \"params\": { \"seed\": " << params.seed << ", \"count\": " << params.count
        << ", \"modelCount\": " << params.modelCount << ", \"spacing\": " << params.spacing
        << ", \"jitter\": " << params.jitter << " },\n";
    out << "  \"instances\": [";
    for (size_t i = 0; i < instances.size(); ++i) {
        const Instance& in = instances[i];
        out << (i == 0 ? "\n" : ",\n");
        out << "    { \"model\": " << in.model << ", \"position\": [" << in.position.x << ", "
            << in.position.y << ", " << in.position.z << "], \"rotation\": [" << in.rotation.w
            << ", " << in.rotation.x << ", " << in.rotation.y << ", " << in.rotation.z << "] }";
    }
    out << (instances.empty() ? "" : "\n  ") << "]\n}\n";
}

glm::vec3 sceneCenter(const std::vector<Instance>& instances) {
    if (instances.empty()) {
        return glm::vec3(0.0f);
    }
    glm::vec3 sum(0.0f);
    for (const Instance& in : instances) {
        sum += in.position;
    }
    return sum / static_cast<float>(instances.size());
}

glm::vec3 sceneHalfExtent(const std::vector<Instance>& instances) {
    const glm::vec3 center = sceneCenter(instances);
    glm::vec3 half(0.0f);
    for (const Instance& in : instances) {
        half = glm::max(half, glm::abs(in.position - center));
    }
    return half;
}

} // namespace lab::scene

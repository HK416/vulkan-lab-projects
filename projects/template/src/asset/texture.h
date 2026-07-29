#pragma once

#include <string>

#include "render/image.h"

namespace lab::render {
class Context;
}

namespace lab::asset {

// Loads a KTX2 texture (BC7 material textures, or IBL cubemaps) to a device-local
// render::GpuImage with its full mip chain. Throws std::runtime_error on failure.
render::GpuImage loadKtx(render::Context& context, const std::string& path);

// Fixed image-based lighting. Precomputed offline and loaded as KTX (no runtime
// prefilter pass), so it stays deterministic. One shared scene resource bound
// once for every condition.
struct Ibl {
    render::GpuImage prefilteredEnv; // specular; mip chain = roughness levels (cubemap)
    render::GpuImage irradiance;     // diffuse (cubemap)
    render::GpuImage brdfLut;        // 2D split-sum BRDF LUT
};

Ibl loadIbl(render::Context& context,
            const std::string& prefilteredEnvPath,
            const std::string& irradiancePath,
            const std::string& brdfLutPath);

} // namespace lab::asset

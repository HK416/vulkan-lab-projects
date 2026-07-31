#version 450

// gl_BaseInstanceARB is THE fixed bindless index path for the whole series:
// it is the only per-draw index available in every A condition (plain draw,
// per-object indirect, multi-draw). Keep it here so A1-A3 reuse this SPIR-V
// unchanged (see ../indirect-rendering-experiment.md section 3).
#extension GL_ARB_shader_draw_parameters : require

// asset::Vertex layout (asset::standardVertexInput()).
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent; // xyz + w handedness
layout(location = 3) in vec2 inUv;

// set 0 = per-frame (camera + light), bound once per frame.
layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 camPos;
    vec4 lightDir;   // direction TO the light (normalized)
    vec4 lightColor; // rgb intensity
    vec4 ambient;    // rgb
} frame;

// Per-object data lives in set 0 next to the frame data, NOT in the material set:
// it is the same in every condition, so this vertex shader is byte-identical for
// A0-A3 x B0/B1 and the SPIR-V hash stays fixed across the series. The model
// matrix has to live here rather than in a push constant because multi-draw has
// no per-draw push point.
struct Object {
    mat4 model;
    uvec4 material; // x = index into materials[], yzw unused (std430 padding)
};
layout(std430, set = 0, binding = 1) readonly buffer Objects {
    Object objects[];
};

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outTangent;
layout(location = 3) out vec2 outUv;
layout(location = 4) flat out uint outMaterial;

void main() {
    Object obj = objects[gl_BaseInstanceARB];

    vec4 world = obj.model * vec4(inPos, 1.0);
    outWorldPos = world.xyz;

    // Instance transforms are rigid (no non-uniform scale), so model's upper 3x3
    // transforms normals correctly — no inverse-transpose needed.
    mat3 nm = mat3(obj.model);
    outNormal = normalize(nm * inNormal);
    outTangent = vec4(normalize(nm * inTangent.xyz), inTangent.w);
    outUv = inUv;
    outMaterial = obj.material.x;

    gl_Position = frame.viewProj * world;
}

#version 450

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

layout(push_constant) uniform Push {
    mat4 model;
} pc;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec4 outTangent;
layout(location = 3) out vec2 outUv;

void main() {
    vec4 world = pc.model * vec4(inPos, 1.0);
    outWorldPos = world.xyz;

    // Instance transforms are rigid (no non-uniform scale), so model's upper 3x3
    // transforms normals correctly — no inverse-transpose needed.
    mat3 nm = mat3(pc.model);
    outNormal = normalize(nm * inNormal);
    outTangent = vec4(normalize(nm * inTangent.xyz), inTangent.w);
    outUv = inUv;

    gl_Position = frame.viewProj * world;
}

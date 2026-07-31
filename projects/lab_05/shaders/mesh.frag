#version 450

// Metallic-roughness PBR (Cook-Torrance GGX), one directional light + a constant
// ambient term. No IBL — that needs prefiltered/irradiance cubemaps (loadIbl),
// which this lab has no assets for yet; the ambient stands in for it.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUv;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 camPos;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambient;
} frame;

// set 1 = per-material (B0), bound on material change.
layout(set = 1, binding = 0) uniform sampler2D baseColorTex;
layout(set = 1, binding = 1) uniform sampler2D metalRoughTex; // glTF: G=rough, B=metal
layout(set = 1, binding = 2) uniform sampler2D normalTex;
layout(set = 1, binding = 3) uniform Material {
    vec4 baseColorFactor;
    vec4 mr; // x = metallic, y = roughness
} material;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

vec3 fresnelSchlick(float cosT, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}

float distributionGGX(float ndh, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float geometrySchlickGGX(float ndx, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return ndx / (ndx * (1.0 - k) + k);
}

void main() {
    vec4 base = texture(baseColorTex, inUv) * material.baseColorFactor;
    vec3 albedo = base.rgb;
    vec2 rm = texture(metalRoughTex, inUv).gb; // G=rough, B=metal
    float roughness = clamp(rm.x * material.mr.y, 0.04, 1.0);
    float metallic = rm.y * material.mr.x;

    // Normal mapping via TBN.
    vec3 N = normalize(inNormal);
    vec3 T = normalize(inTangent.xyz);
    vec3 B = cross(N, T) * inTangent.w;
    vec3 nTex = texture(normalTex, inUv).xyz * 2.0 - 1.0;
    N = normalize(mat3(T, B, N) * nTex);

    vec3 V = normalize(frame.camPos.xyz - inWorldPos);
    vec3 L = normalize(frame.lightDir.xyz);
    vec3 H = normalize(V + L);
    float ndl = max(dot(N, L), 0.0);
    float ndv = max(dot(N, V), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float ndf = distributionGGX(max(dot(N, H), 0.0), roughness);
    float g = geometrySchlickGGX(ndv, roughness) * geometrySchlickGGX(ndl, roughness);
    vec3 f = fresnelSchlick(max(dot(H, V), 0.0), f0);

    vec3 specular = (ndf * g * f) / (4.0 * ndv * ndl + 0.0001);
    vec3 kd = (1.0 - f) * (1.0 - metallic);
    vec3 diffuse = kd * albedo / PI;

    vec3 lo = (diffuse + specular) * frame.lightColor.rgb * ndl;
    vec3 color = frame.ambient.rgb * albedo + lo;

    // Swapchain is UNORM (not SRGB), so tonemap + gamma-encode here.
    color = color / (color + vec3(1.0)); // Reinhard
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, base.a);
}

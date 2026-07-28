#version 450

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec3 outColor;

layout(push_constant) uniform PushConstants {
    mat4 projView;
    mat4 model;
} pc;

void main() {
    outColor = inPos + vec3(0.5);
    gl_Position = pc.projView * pc.model * vec4(inPos, 1.0);
}

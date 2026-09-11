#version 450
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform PC {
    vec4 blockPos;
} pc;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;
} cam;

layout(location = 0) out vec4 vColor;

void main() {
    vec3 world = pc.blockPos.xyz + inPos * 1.004;
    gl_Position = cam.viewProj * vec4(world, 1.0);
    vColor = inColor;
}
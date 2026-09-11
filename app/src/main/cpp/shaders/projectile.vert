#version 450
layout(location = 0) in vec3 inPos;

layout(location = 1) in vec3 iPos;
layout(location = 2) in vec3 iSize;
layout(location = 3) in vec4 iColor;
layout(location = 4) in float iYaw;

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
    vec3 local = inPos * iSize;
    vec3 world = iPos + local;
    gl_Position = cam.viewProj * vec4(world, 1.0);
    vColor = iColor;
}
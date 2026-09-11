#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUv;          // координата в тайлах, может быть > 1
layout(location = 2) in vec2 inTileOrigin;  // левый верхний угол тайла в атласе
layout(location = 3) in vec4 inColor;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;
} cam;

layout(location = 0) out vec2      vUv;
layout(location = 1) out vec4      vColor;
layout(location = 2) out vec3      vWorldPos;
layout(location = 3) out flat vec2 vTileOrigin;

void main() {
    gl_Position = cam.viewProj * vec4(inPos, 1.0);
    vUv         = inUv;
    vColor      = inColor;
    vWorldPos   = inPos;
    vTileOrigin = inTileOrigin;
}

#version 450
layout(location = 0) in vec3 inPos;        // unit cube in [-0.5, 0.5]

// Instance
layout(location = 1) in vec3 iPos;
layout(location = 2) in vec3 iSize;
layout(location = 3) in vec4 iColor;       // packed u8x4 UNORM
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
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vWorldPos;

void main() {
    // Scale
    vec3 local = inPos * iSize;

    // Yaw around Y
    float c = cos(iYaw), s = sin(iYaw);
    vec3 rotated = vec3(local.x * c + local.z * s, local.y, -local.x * s + local.z * c);

    vec3 world = iPos + rotated;
    gl_Position = cam.viewProj * vec4(world, 1.0);

    // Normal approximation from cube face
    float cN = cos(iYaw), sN = sin(iYaw);
    vec3 n = vec3(inPos.x * cN + inPos.z * sN, inPos.y, -inPos.x * sN + inPos.z * cN);
    vNormal = normalize(n);

    vColor = iColor;
    vWorldPos = world;
}
#version 450

layout(location = 0) in vec3 inPos;       // трапеция, y от 0 (земля) до 1

layout(location = 1) in vec3  iPos;       // per-instance
layout(location = 2) in float iScale;
layout(location = 3) in vec4  iColor;
layout(location = 4) in float iYaw;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;   // start, end, timeOfDay, time
    vec4 skyColor;
} cam;

layout(location = 0) out vec4  vColor;
layout(location = 1) out vec3  vWorldPos;
layout(location = 2) out float vHeight;   // 0 у земли, 1 у верхушки

void main() {
    float c = cos(iYaw), s = sin(iYaw);
    vec3 local = vec3(inPos.x * c - inPos.z * s, inPos.y, inPos.x * s + inPos.z * c);

    // Сгиб по ветру: верхушка качается, основание стоит. Фаза
    // привязана к мировой позиции, иначе вся поляна колышется в такт.
    float bend = local.y * local.y;
    float w = sin(cam.fogParams.w * 1.5 + iPos.x * 0.4 + iPos.z * 0.3) * 0.12;
    local.x += bend * w;
    local.z += bend * w * 0.5;

    local.y *= iScale;
    local.x *= iScale * 0.9;
    local.z *= iScale * 0.9;

    vec3 world = iPos + local;
    gl_Position = cam.viewProj * vec4(world, 1.0);

    vColor    = iColor;
    vWorldPos = world;
    vHeight   = inPos.y;
}

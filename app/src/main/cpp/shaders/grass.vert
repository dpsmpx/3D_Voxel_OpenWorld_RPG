#version 450

layout(location = 0) in vec3 inPos;      // per-vertex
layout(location = 1) in vec2 inUv;       // per-vertex

layout(location = 3) in vec3 iPos;       // per-instance
layout(location = 4) in float iScale;
layout(location = 5) in vec2 iUvOrigin;
layout(location = 6) in vec4 iColor;
layout(location = 7) in float iYaw;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;
    vec4 skyColor;
} cam;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vWorldPos;

void main() {
    // Поворот вокруг Y
    float c = cos(iYaw), s = sin(iYaw);
    vec3 local = vec3(inPos.x * c - inPos.z * s, inPos.y, inPos.x * s + inPos.z * c);

    // Сгиб по ветру: верхние вершины качаются, привязано к мировой позиции
    float bend = local.y;             // 0 снизу, 1 сверху
    float w = sin(cam.fogParams.w * 1.5 + iPos.x * 0.4 + iPos.z * 0.3) * 0.10;
    local.x += bend * w;
    local.z += bend * w * 0.5;

    // Масштаб по вертикали (для разнообразия)
    local.y *= iScale;
    local.x *= iScale * 0.9;
    local.z *= iScale * 0.9;

    vec3 world = iPos + local;
    gl_Position = cam.viewProj * vec4(world, 1.0);

    // UV тайла
    vUv = iUvOrigin + inUv * (1.0 / 16.0);
    // Внутренний отступ от краёв тайла — чтобы mip не захватывал соседей
    vUv = clamp(vUv, iUvOrigin + 0.5/512.0, iUvOrigin + (1.0/16.0) - 0.5/512.0);

    vColor = iColor;
    vWorldPos = world;
}

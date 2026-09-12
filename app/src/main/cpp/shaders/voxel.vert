#version 450

// Одно 32-битное слово на вершину. Раскладка — см. VoxelVertex
// в mesh_builder.h; там же лежит packVoxelPos, который её собирает.
layout(location = 0) in uint inPacked;
layout(location = 1) in vec4 inColor;   // цвет материала грани, sRGB

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;
    vec4 skyColor;
} cam;

// Позиции вершин локальны для чанка, мировые собираются здесь.
// Так они влезают в биты, и заодно у камеры не тает точность на
// дальних чанках: float складывает большое с малым уже в шейдере,
// а не в самих данных.
layout(push_constant) uniform Push {
    vec4 chunkOrigin;
} pc;

layout(location = 0) out vec4      vColor;
layout(location = 1) out vec3      vWorldPos;
layout(location = 2) out vec2      vShade;  // x — затенение углов, y — открытость неба
layout(location = 3) out flat uint vInfo;   // грань в младших 3 битах, зерно выше

void main() {
    vec3 local = vec3(float( inPacked        & 63u),
                      float((inPacked >>  6) & 255u),
                      float((inPacked >> 14) & 63u));
    uint face  = (inPacked >> 20) & 7u;
    uint ao    = (inPacked >> 23) & 3u;
    uint sky   = (inPacked >> 25) & 7u;
    uint grain = (inPacked >> 28) & 15u;

    vec3 world = pc.chunkOrigin.xyz + local;
    gl_Position = cam.viewProj * vec4(world, 1.0);

    vColor    = inColor;
    vWorldPos = world;
    vShade    = vec2(float(ao) * (1.0 / 3.0), float(sky) * (1.0 / 7.0));
    vInfo     = face | (grain << 3);
}

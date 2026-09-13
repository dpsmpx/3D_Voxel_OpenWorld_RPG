#version 450
// Отладочные виды для tools/vkcheck. Вершинный шейдер берётся
// НАСТОЯЩИЙ (shaders/voxel.vert): проверяется то, что до фрагмента
// реально доезжает, а не то, что мы думаем, будто туда кладём.
precision mediump float;
precision mediump int;

layout(location = 0) in       vec4  vColor;
layout(location = 1) in highp vec3  vWorldPos;
layout(location = 2) in       vec2  vShade;
layout(location = 3) in flat  uint  vInfo;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;
    vec4 skyColor;
} cam;

layout(location = 0) out vec4 outColor;

const vec3 FACE_C[6] = vec3[6](
    vec3(1.0, 0.2, 0.2), vec3(0.6, 0.1, 0.1),
    vec3(0.2, 1.0, 0.2), vec3(0.1, 0.5, 0.1),
    vec3(0.2, 0.4, 1.0), vec3(0.1, 0.2, 0.6));

void main() {
#if MODE == 0          // цвет материала, как он пришёл в вершине
    outColor = vec4(vColor.rgb, 1.0);
#elif MODE == 1        // открытость неба: чёрное — «под сводом»
    outColor = vec4(vec3(vShade.y), 1.0);
#elif MODE == 2        // затенение углов
    outColor = vec4(vec3(vShade.x), 1.0);
#elif MODE == 3        // номер грани
    outColor = vec4(FACE_C[int(vInfo & 7u)], 1.0);
#elif MODE == 4        // расстояние до камеры, 0..256 блоков
    highp float d = length(vWorldPos - cam.cameraPos.xyz);
    outColor = vec4(vec3(clamp(d / 256.0, 0.0, 1.0)), 1.0);
#else                  // мировая высота, 0..128
    outColor = vec4(vec3(clamp(vWorldPos.y / 128.0, 0.0, 1.0)), 1.0);
#endif
}

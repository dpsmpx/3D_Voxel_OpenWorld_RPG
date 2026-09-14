#version 450
// Полноэкранный треугольник, подающий воксельному фрагментному
// шейдеру те же входы, что даёт настоящий voxel.vert.
//
// Нужен затем, что мерить надо ФРАГМЕНТ: именно он считается на
// каждый пиксель и именно его мы меняем. Настоящая геометрия для
// этого не годится — она закрывает лишь часть экрана и разную от
// кадра к кадру, и сравнивать «до» и «после» было бы не с чем.
//
// Мировая координата растянута по экрану, чтобы fract и fwidth в
// фаске делали настоящую работу, а не считали одну и ту же точку.

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;
    vec4 skyColor;
} cam;

layout(location = 0) out vec4      vColor;
layout(location = 1) out vec3      vWorldPos;
layout(location = 2) out vec2      vShade;
layout(location = 3) out flat uint vInfo;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);

    // Цвет травы из реестра блоков, непрозрачный. С -DWATER — цвет
    // воды с её альфой: в воксельном шейдере полупрозрачная грань
    // уходит в ветку с бликом и Френелем, и без такого варианта её
    // цену не с чем сравнить.
#ifdef WATER
    vColor    = vec4(0.227, 0.459, 0.737, 0.627);
#else
    vColor    = vec4(0.486, 0.729, 0.329, 1.0);
#endif
    // Сто блоков по горизонтали и столько же в глубину: клетка
    // получается в несколько пикселей, как на настоящем склоне.
    vWorldPos = vec3(uv.x * 100.0, 40.0 + uv.y * 20.0, uv.y * 100.0);
    vShade    = vec2(0.75, 0.9);
    vInfo     = 2u;               // верхняя грань
}

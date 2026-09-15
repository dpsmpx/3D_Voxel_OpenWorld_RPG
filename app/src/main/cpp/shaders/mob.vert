#version 450
layout(location = 0) in vec3 inPos;        // единичный куб в [-0.5, 0.5]

// Инстанс
layout(location = 1) in vec3 iPos;
layout(location = 2) in vec3 iSize;
layout(location = 3) in vec4 iColor;       // packed u8x4 UNORM
layout(location = 4) in vec4 iRot;      // кватернион (x, y, z, w)

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;   // start, end, timeOfDay, time
    vec4 skyColor;     // цвет неба, sRGB; w — доля дня, 0 ночь, 1 день
    // Свет суток, посчитанный ОДИН раз за кадр в render::Camera::toUbo.
    // Раньше каждый фрагментный шейдер считал это сам, на каждый
    // пиксель, хотя зависит оно только от uniform. См. CameraUbo.
    vec4 sunLight;     // rgb — цвет солнца, линейный; w — день x над горизонтом
    vec4 ambLight;     // rgb — оттенок рассеянного света; w — его сила
    vec4 skyLinear;    // rgb — цвет неба, линейный; w — солнце над горизонтом
} cam;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vWorldPos;

// Куб задан 24 вершинами — по четыре на грань, в порядке
// -Z, +Z, -X, +X, -Y, +Y (см. CUBE_V в mob_renderer.cpp). Значит
// номер грани это просто gl_VertexIndex / 4, и нормаль у неё честная,
// плоская. Раньше за нормаль брали направление на угол куба, и грани
// получались скруглёнными, будто это не кубы, а мятые шарики.
const vec3 CUBE_N[6] = vec3[6](
    vec3( 0.0,  0.0, -1.0), vec3( 0.0,  0.0,  1.0),
    vec3(-1.0,  0.0,  0.0), vec3( 1.0,  0.0,  0.0),
    vec3( 0.0, -1.0,  0.0), vec3( 0.0,  1.0,  0.0));

// Поворот вектора кватернионом. Дешевле, чем строить матрицу:
// две векторных произведения на вершину.
vec3 qrot(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
    // Раньше здесь был поворот вокруг Y на один угол: повёрнутую
    // конечность он выразить не мог, и анимация сдвигала ногу
    // параллельно себе вместо качания в суставе.
    vec3 local = inPos * iSize;
    vec3 world = iPos + qrot(iRot, local);
    gl_Position = cam.viewProj * vec4(world, 1.0);

    vec3 n = CUBE_N[clamp(gl_VertexIndex >> 2, 0, 5)];
    vNormal = qrot(iRot, n);

    vColor = iColor;
    vWorldPos = world;
}

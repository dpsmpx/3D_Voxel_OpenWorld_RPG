#version 450
// Воксельная модель: меш из мелких вокселей, по экземпляру на предмет.
//
// Меш модели задан в её собственных координатах (стоит на Y = 0),
// экземпляр ставит его в мир: где, как повёрнут и во сколько раз
// увеличен. Фрагментный шейдер — mob.frag: модели освещаются ровно
// как существа и блоки рядом, и заводить третью копию освещения
// незачем.
layout(location = 0) in vec3 inPos;       // вершина модели
layout(location = 1) in vec4 inColor;     // цвет вокселя, u8x4 UNORM
layout(location = 2) in vec3 inNormal;    // осевая нормаль грани

// Экземпляр
layout(location = 3) in vec3 iPos;
layout(location = 4) in float iScale;
layout(location = 5) in vec4 iRot;        // кватернион (x, y, z, w)
layout(location = 6) in vec4 iTint;       // множитель цвета, u8x4 UNORM

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
    // Погода. weather: x — доля неба под тучами, y — сила осадков,
    // z — яркость радуги, w — снег (0 дождь, 1 снег).
    // wind: xy — ветер в блоках в секунду, z — время в секундах,
    // w — зерно крапчатости блоков (см. render::tintSeedOf).
    vec4 weather;
    vec4 wind;

    // ---- Точечные источники: факелы, фонари, огонь в руке ----
    //
    // lightInfo.x — сколько источников прислали. Ноль — цикл в
    // шейдере не выполняется ни разу.
    vec4 lightInfo;
    vec4 lightPos[8];    // xyz — где, w — радиус гашения
    vec4 lightColor[8];  // rgb — цвет, линейный; w — яркость
} cam;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vWorldPos;

// Поворот вектора кватернионом — та же формула, что в mob.vert.
vec3 qrot(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
    vec3 world = iPos + qrot(iRot, inPos * iScale);
    gl_Position = cam.viewProj * vec4(world, 1.0);
    vNormal   = qrot(iRot, inNormal);
    vColor    = inColor * iTint;
    vWorldPos = world;
}

#version 450
layout(location = 0) in vec3 inPos;

layout(location = 1) in vec3 iPos;
layout(location = 2) in vec3 iSize;
layout(location = 3) in vec4 iColor;
layout(location = 4) in vec4 iRot;      // кватернион (x, y, z, w)

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;
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

// Поворот вектора кватернионом — дословно как в mob.vert.
vec3 qrot(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
    // Инстанс общий с мобами (MobInstance), и поворот у него
    // кватернион. Раньше здесь объявлялся float iYaw, который
    // шейдер даже не читал: стрела летела боком, а после перехода
    // формата на кватернион этот float читал бы байты выравнивания.
    vec3 local = inPos * iSize;
    vec3 world = iPos + qrot(iRot, local);
    gl_Position = cam.viewProj * vec4(world, 1.0);
    vColor = iColor;
}

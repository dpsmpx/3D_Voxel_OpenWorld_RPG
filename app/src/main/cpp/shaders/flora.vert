#version 450
// Растения и мелкие природные вещи: общие модели из мелких вокселей,
// по экземпляру на растение (render/flora_renderer.h).
//
// Экземпляр — 16 байт: где стоит и четыре байта параметров. Поворот
// только вокруг вертикали — растения стоят, а не лежат, — и
// кватернион тут был бы лишним весом в буфере, который пишется
// каждый кадр. Фрагментный шейдер — mob.frag: растения освещаются так
// же, как существа и блоки рядом.
layout(location = 0) in vec3 inPos;       // вершина модели
layout(location = 1) in vec4 inColor;     // цвет вокселя, u8x4 UNORM
layout(location = 2) in vec3 inNormal;    // осевая нормаль грани

// Экземпляр
layout(location = 3) in vec3 iPos;        // основание в мире
// x — поворот (доля оборота), y — размер (доля FLORA_MAX_SCALE),
// z — гибкость на ветру, w — оттенок (0.5 — как есть).
layout(location = 4) in vec4 iParams;

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

// Совпадает с render::FLORA_MAX_SCALE.
const float MAX_SCALE = 1.5;

void main() {
    float yaw = iParams.x * 6.2831853;
    float c = cos(yaw), s = sin(yaw);
    vec3 p = inPos * (iParams.y * MAX_SCALE);
    p = vec3(c * p.x + s * p.z, p.y, -s * p.x + c * p.z);
    vec3 n = vec3(c * inNormal.x + s * inNormal.z, inNormal.y,
                  -s * inNormal.x + c * inNormal.z);

    // Ветер: макушка отходит по ветру, основание стоит. Изгиб растёт
    // с квадратом высоты, порывы — две синусоиды со сдвигом фазы по
    // месту: соседние пучки качаются вразнобой, а не строем.
    float h = max(p.y, 0.0);
    float phase = dot(iPos.xz, vec2(0.37, 0.23));
    float t = cam.wind.z;
    float gust = sin(t * 1.7 + phase) * 0.6 + sin(t * 3.1 + phase * 1.7) * 0.25 + 0.4;
    vec2 dir = cam.wind.xy * 0.05 + vec2(0.03, 0.02);
    p.xz += dir * (iParams.z * h * h * gust);

    vec3 world = iPos + p;
    gl_Position = cam.viewProj * vec4(world, 1.0);
    vNormal   = n;
    vColor    = vec4(inColor.rgb * (0.8 + iParams.w * 0.4), 1.0);
    vWorldPos = world;
}

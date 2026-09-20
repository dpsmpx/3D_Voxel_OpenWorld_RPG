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
// Биты 0..2 — индекс грани, 0..5; биты 3..4 — класс крапчатости,
// 0..3 (см. world::BlockDef::colorJitter).
layout(location = 3) out flat uint vInfo;

// Зерно крапчатости, свёрнутое в десять бит.
//
// Свёртка живёт здесь по двум причинам сразу.
//
// Первая: зерно приходит из блока формы как float, а фрагментный
// шейдер начинается с precision mediump float — поля блока формы в
// нём mediump, и от двадцати четырёх бит остались бы старшие.
// Вершинный шейдер precision не объявляет вовсе, здесь перевод точен.
//
// Вторая: десять бит доедут целыми при любой трактовке точности
// межстадийной переменной, а двадцать четыре — не обязаны. Тысяча с
// лишним разных крапов больше, чем когда-нибудь понадобится.
//
// Свёртка складывает ВСЕ биты зерна, а не берёт младшие: два мира,
// различающиеся только старшими битами, обязаны выглядеть по-разному.
layout(location = 4) out flat uint vSeed;

void main() {
    vec3 local = vec3(float( inPacked        & 63u),
                      float((inPacked >>  6) & 255u),
                      float((inPacked >> 14) & 63u));
    uint face  = (inPacked >> 20) & 7u;
    uint ao    = (inPacked >> 23) & 3u;
    uint sky   = (inPacked >> 25) & 7u;
    uint jit   = (inPacked >> 28) & 3u;

    vec3 world = pc.chunkOrigin.xyz + local;
    gl_Position = cam.viewProj * vec4(world, 1.0);

    vColor    = inColor;
    vWorldPos = world;
    vShade    = vec2(float(ao) * (1.0 / 3.0), float(sky) * (1.0 / 7.0));
    vInfo     = face | (jit << 3);
    uint seed = uint(cam.wind.w);
    vSeed     = (seed ^ (seed / 1024u) ^ (seed / 1048576u)) & 1023u;
}

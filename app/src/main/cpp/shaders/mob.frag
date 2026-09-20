#version 450
// Точность объявлена ЯВНО. По умолчанию здесь и так всё highp, но
// молча: стоит появиться хоть одному highp в тексте, и компилятор
// начинает об этом предупреждать, а предупреждение в шейдере тут
// считается ошибкой. Объявление ничего не меняет и снимает вопрос.
precision highp float;
precision highp int;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vWorldPos;

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
    // У BlockDef с самого начала были isEmissive и lightLevel, и
    // фонарь объявлен с уровнем 13. Читать их было некому: ночью и в
    // пещере фонарь светил ровно столько же, сколько булыжник.
    //
    // lightInfo.x — сколько источников прислали. Ноль — цикл не
    // выполняется ни разу, и днём в чистом поле это ничего не стоит.
    vec4 lightInfo;
    vec4 lightPos[8];    // xyz — где, w — радиус гашения
    vec4 lightColor[8];  // rgb — цвет, линейный; w — яркость
} cam;

layout(location = 0) out vec4 outColor;

// Свет от точечных источников в точке P с нормалью N.
//
// Затухание квадратичное по доле пройденного радиуса, без корня:
// inversesqrt даёт и направление, и обратное расстояние за одну
// операцию. Ламберт приподнят до 0.25 — факел светит и на грани,
// отвёрнутые от него: иначе стена, вдоль которой он висит, остаётся
// чёрной, хотя пламя в полуметре.
vec3 pointLights(highp vec3 P, vec3 N, float occl) {
    int n = int(cam.lightInfo.x + 0.5);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < n; ++i) {
        highp vec3 d = cam.lightPos[i].xyz - P;
        highp float r = cam.lightPos[i].w;
        highp float d2 = dot(d, d);
        if (d2 >= r * r) continue;
        highp float inv = inversesqrt(max(d2, 1e-6));
        vec3 L = vec3(d * inv);
        float att = clamp(1.0 - 1.0 / (inv * r), 0.0, 1.0);
        att *= att;
        float ndl = max(dot(N, L), 0.0) * 0.75 + 0.25;
        sum += cam.lightColor[i].rgb * (cam.lightColor[i].w * att * ndl);
    }
    return sum * occl;
}


vec3 toLinear(vec3 c) { return c * c; }
vec3 toSrgb(vec3 c)   { return sqrt(max(c, vec3(0.0))); }

vec3 shoulder(vec3 c) {
    float m = max(max(c.r, c.g), c.b);
    if (m <= 0.75) return c;
    float o = m - 0.75;
    return c * ((0.75 + o / (1.0 + o * 2.0)) / m);
}

void main() {
    // Та же модель освещения и те же числа, что у террейна
    // (shaders/voxel.frag). Существа — такие же кубы, стоящие на тех
    // же блоках: стоит освещению разойтись, и они выглядят вырезанными
    // из другой игры.
    //
    // Заявлено это было и раньше, а на деле рассеянный свет считался
    // от самого skyLin, солнце было вдвое сильнее террейнового, и
    // полусфера по нормали жила только здесь. За тем, чтобы числа
    // снова не разошлись, следит тест «мир освещён по одной модели».
    vec3 N = normalize(vNormal);

    vec3  sunTint = cam.sunLight.rgb;
    float sunUp   = cam.sunLight.w;
    vec3  skyLin  = cam.skyLinear.rgb;
    float above   = cam.skyLinear.w;
    vec3  ambTint = cam.ambLight.rgb;

    // Освещённость грани — та же таблица, что FACE_LIGHT в террейне,
    // только выбранная по нормали: модели существ собраны из кубов, и
    // их грани обязаны ложиться в тот же ряд оттенков, что блоки под
    // ногами. На осевой нормали выражение даёт ровно табличное
    // значение: верх 1.00, низ 0.50, ±X 0.76, ±Z 0.88.
    float faceLight = mix(mix(0.76, 0.88, abs(N.z)),
                          mix(0.50, 1.00, step(0.0, N.y)),
                          abs(N.y));

    vec3 ambient = ambTint * cam.ambLight.w;
    vec3 sun     = sunTint * (max(dot(N, cam.sunDir.xyz), 0.0) * 0.46 * sunUp);

    // Подсветка по краю силуэта: без неё тёмная фигура сливается с
    // тенью, и моба замечаешь только когда он уже бьёт.
    vec3  toEye = cam.cameraPos.xyz - vWorldPos;
    float rimB  = 1.0 - clamp(dot(N, normalize(toEye)), 0.0, 1.0);
    float rim   = rimB * rimB * rimB;

    vec3 torch = pointLights(vWorldPos, N, 1.0);
    vec3 lit = toLinear(vColor.rgb) * (faceLight * (ambient + sun + torch))
             + skyLin * rim * 0.22;
    // Плечо нужно из-за подсветки края: она прибавляется поверх
    // освещения и на светлой шкуре выбивает силуэт в белое пятно.
    lit = shoulder(lit);

    vec3  toFrag = vWorldPos - cam.cameraPos.xyz;
    float dist   = length(toFrag);
    float fogAmt = clamp((dist - cam.fogParams.x) /
                         max(cam.fogParams.y - cam.fogParams.x, 0.001), 0.0, 1.0);
    fogAmt = fogAmt * fogAmt * (3.0 - 2.0 * fogAmt);

    float sunAmt = max(dot(normalize(toFrag), cam.sunDir.xyz), 0.0);
    // Показатель и вес те же, что у тумана террейна.
    float s2 = sunAmt * sunAmt;
    float s8 = s2 * s2 * s2 * s2;
    vec3  fogColor = mix(skyLin, sunTint, clamp(s8 * 0.30 * above, 0.0, 1.0));

    outColor = vec4(clamp(toSrgb(mix(lit, fogColor, fogAmt)), 0.0, 1.0), vColor.a);
}

#version 450
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
} cam;

layout(location = 0) out vec4 outColor;

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

    vec3 lit = toLinear(vColor.rgb) * (faceLight * (ambient + sun))
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

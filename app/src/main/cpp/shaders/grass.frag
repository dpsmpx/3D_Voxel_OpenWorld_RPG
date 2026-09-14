#version 450

layout(location = 0) in vec4  vColor;
layout(location = 1) in vec3  vWorldPos;
layout(location = 2) in float vHeight;

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

layout(location = 0) out vec4 outColor;

vec3 toLinear(vec3 c) { return c * c; }
vec3 toSrgb(vec3 c)   { return sqrt(max(c, vec3(0.0))); }

void main() {
    // Градиент по высоте: у земли темнее, к верхушке светлее и чуть
    // желтее. Это и есть замена текстуре — форму задаёт геометрия,
    // объём задаёт градиент.
    vec3 tip  = toLinear(vColor.rgb) * vec3(1.35, 1.30, 0.85);
    vec3 base = toLinear(vColor.rgb) * 0.45;
    vec3 albedo = mix(base, tip, vHeight * vHeight);

    // Освещение — та же модель и те же числа, что у террейна
    // (shaders/voxel.frag). Трава растёт прямо на нём, и любое
    // расхождение здесь видно как полоса чужого оттенка у самой земли.
    //
    // Раньше расхождение и было: у травы рассеянный свет считался от
    // самого skyLin, у террейна — от приглушённого к белому, и сила
    // солнца отличалась в полтора раза. За тем, чтобы числа снова не
    // разошлись, следит тест «мир освещён по одной модели».
    // Свет суток приходит готовым из CameraUbo: считает его
    // render::Camera::toUbo, раз в кадр, теми же выражениями, что
    // стояли здесь. Одна формула на террейн, траву, существ и небо.
    vec3  sunTint = cam.sunLight.rgb;
    float sunUp   = cam.sunLight.w;      // день x над горизонтом
    vec3  skyLin  = cam.skyLinear.rgb;
    float above   = cam.skyLinear.w;

    // Пучок — билборд без своей нормали: считаем его открытым небу и
    // освещённым, как верхняя грань блока.
    vec3 ambient = cam.ambLight.rgb * cam.ambLight.w;
    vec3 sun     = sunTint * (0.46 * sunUp);

    vec3 lit = albedo * (ambient + sun);

    vec3  toFrag = vWorldPos - cam.cameraPos.xyz;
    float dist   = length(toFrag);
    float fogAmt = clamp((dist - cam.fogParams.x) /
                         max(cam.fogParams.y - cam.fogParams.x, 0.001), 0.0, 1.0);
    fogAmt = fogAmt * fogAmt * (3.0 - 2.0 * fogAmt);

    float sunAmt = max(dot(normalize(toFrag), cam.sunDir.xyz), 0.0);
    // Восьмая степень — тремя умножениями, без логарифма: основание
    // обнуляется на любой грани, отвёрнутой от солнца. Показатель и
    // вес те же, что у тумана террейна: иначе на закате горизонт под
    // травой и над ней окрашен по-разному.
    float s2 = sunAmt * sunAmt;
    float s8 = s2 * s2 * s2 * s2;
    vec3  fogColor = mix(skyLin, sunTint, clamp(s8 * 0.30 * above, 0.0, 1.0));

    outColor = vec4(clamp(toSrgb(mix(lit, fogColor, fogAmt)), 0.0, 1.0), 1.0);
}

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
    vec4 fogParams;
    vec4 skyColor;
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

    // Трава — билборд без своей нормали. Считаем её открытой небу и
    // подсвечиваем на просвет, как это делает листва.
    float day    = clamp(cam.sunDir.w, 0.0, 1.0);
    float above  = smoothstep(-0.10, 0.06, cam.sunDir.y);
    vec3  sunTint = toLinear(mix(vec3(1.00, 0.52, 0.26), vec3(1.00, 0.97, 0.92),
                                 smoothstep(0.0, 0.30, cam.sunDir.y)));
    vec3  skyLin  = toLinear(cam.skyColor.rgb);
    const float skyVis = 0.85;

    vec3 ambient = skyLin * (0.16 + 0.34 * skyVis) * (0.25 + 0.75 * day);
    vec3 sun     = sunTint * (0.62 * day * above);
    vec3 moon    = vec3(0.04, 0.055, 0.11) * (1.0 - day) * (0.30 + 0.35 * skyVis);

    vec3 lit = albedo * (ambient + sun + moon + 0.02);

    vec3  toFrag = vWorldPos - cam.cameraPos.xyz;
    float dist   = length(toFrag);
    float fogAmt = clamp((dist - cam.fogParams.x) /
                         max(cam.fogParams.y - cam.fogParams.x, 0.001), 0.0, 1.0);
    fogAmt = fogAmt * fogAmt * (3.0 - 2.0 * fogAmt);

    float sunAmt   = max(dot(normalize(toFrag), cam.sunDir.xyz), 0.0);
    // Шестая степень — тремя умножениями, без логарифма: основание
    // обнуляется на любой грани, отвёрнутой от солнца.
    float s2 = sunAmt * sunAmt;
    float s6 = s2 * s2 * s2;
    vec3  fogColor = mix(skyLin, sunTint, clamp(s6 * 0.45 * above, 0.0, 1.0));

    outColor = vec4(toSrgb(mix(lit, fogColor, fogAmt)), 1.0);
}

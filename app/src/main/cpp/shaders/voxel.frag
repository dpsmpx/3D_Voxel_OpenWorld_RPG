#version 450
// Мобильный GPU: mediump достаточно для цвета и тумана, оставляем
// highp только координатам, где точность действительно нужна.
precision mediump float;
// int по умолчанию тоже задаём явно: иначе glslang предупреждает,
// что часть точностей осталась highp по умолчанию.
precision mediump int;

layout(location = 0) in highp vec2 vUv;
layout(location = 1) in       vec4 vColor;
layout(location = 2) in highp vec3 vWorldPos;
layout(location = 3) in flat  vec2 vTileOrigin;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;   // start, end, density, time
    vec4 skyColor;
} cam;

layout(set = 0, binding = 1) uniform sampler2D atlas;

layout(location = 0) out vec4 outColor;

const float TILE = 1.0 / 16.0;   // атлас 16x16
const float INSET = 0.5 / 512.0; // половина тексела: гасит просачивание соседей

// Нормали граней в порядке world::FACES: +X, -X, +Y, -Y, +Z, -Z.
// Индекс грани приезжает в альфе вершинного цвета — см. mesh_builder.
const vec3 FACE_N[6] = vec3[6](
    vec3( 1.0,  0.0,  0.0), vec3(-1.0,  0.0,  0.0),
    vec3( 0.0,  1.0,  0.0), vec3( 0.0, -1.0,  0.0),
    vec3( 0.0,  0.0,  1.0), vec3( 0.0,  0.0, -1.0));

void main() {
    // Греди-меширование объединяет соседние грани в один квад, поэтому
    // vUv растёт до размера квада в блоках. fract() повторяет тайл;
    // производные берём от неразрывной vUv, иначе на швах ломается mip.
    highp vec2 tiled = fract(vUv);
    highp vec2 uv    = vTileOrigin + clamp(tiled, INSET, 1.0 - INSET) * TILE;
    highp vec2 ddx   = dFdx(vUv) * TILE;
    highp vec2 ddy   = dFdy(vUv) * TILE;

    vec4 tex = textureGrad(atlas, uv, ddx, ddy);

    vec3 N = FACE_N[clamp(int(vColor.a * 8.0), 0, 5)];

    // Яркость неба из world::DayCycle: 0.12 ночью, 1.0 днём.
    float day = clamp(cam.sunDir.w, 0.0, 1.0);
    // Солнце у горизонта светит тёплым, в зените — почти белым.
    vec3 sunTint = mix(vec3(1.00, 0.52, 0.26), vec3(1.00, 0.97, 0.92),
                       smoothstep(0.0, 0.30, cam.sunDir.y));
    // Ниже горизонта солнце не светит вовсе, но гаснет не мгновенно.
    float above = smoothstep(-0.10, 0.06, cam.sunDir.y);

    // Небо светит сверху: грань тем светлее, чем больше купола видит.
    float skyVis = 0.5 + 0.5 * N.y;

    vec3 ambient = cam.skyColor.rgb * (0.12 + 0.26 * skyVis) * (0.25 + 0.75 * day);
    vec3 sun     = sunTint * (max(dot(N, cam.sunDir.xyz), 0.0) * 0.68 * day * above);
    // Ночью светит луна: холодно и слабо, но мир остаётся читаемым.
    vec3 moon    = vec3(0.20, 0.24, 0.36) * (1.0 - day) * (0.30 + 0.35 * skyVis);

    vec3 lit = tex.rgb * vColor.rgb * (ambient + sun + moon + 0.04);

    // Туман. У солнца он подсвечивается: закат тогда виден не только
    // на небе, но и в дымке над землёй.
    highp vec3 toFrag = vWorldPos - cam.cameraPos.xyz;
    float dist     = length(toFrag);
    float fogStart = cam.fogParams.x;
    float fogEnd   = cam.fogParams.y;
    float fogAmt   = clamp((dist - fogStart) / max(fogEnd - fogStart, 0.001), 0.0, 1.0);
    fogAmt = fogAmt * fogAmt * (3.0 - 2.0 * fogAmt);   // smoothstep

    float sunAmt  = max(dot(normalize(toFrag), cam.sunDir.xyz), 0.0);
    vec3  fogColor = mix(cam.skyColor.rgb, sunTint,
                         pow(sunAmt, 6.0) * 0.45 * above);

    outColor = vec4(mix(lit, fogColor, fogAmt), 1.0);
}

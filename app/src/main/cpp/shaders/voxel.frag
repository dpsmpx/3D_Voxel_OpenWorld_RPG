#version 450
// Мобильный GPU: mediump достаточно для цвета и тумана, оставляем
// highp только координатам, где точность действительно нужна.
precision mediump float;

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

void main() {
    // Греди-меширование объединяет соседние грани в один квад, поэтому
    // vUv растёт до размера квада в блоках. fract() повторяет тайл;
    // производные берём от неразрывной vUv, иначе на швах ломается mip.
    highp vec2 tiled = fract(vUv);
    highp vec2 uv    = vTileOrigin + clamp(tiled, INSET, 1.0 - INSET) * TILE;
    highp vec2 ddx   = dFdx(vUv) * TILE;
    highp vec2 ddy   = dFdy(vUv) * TILE;

    vec4 tex = textureGrad(atlas, uv, ddx, ddy);

    // vColor — запечённое направленное освещение грани;
    // sunDir.w — текущая яркость неба из world::DayCycle.
    vec3 lit = tex.rgb * vColor.rgb * max(cam.sunDir.w, 0.12);

    float dist     = distance(vWorldPos, cam.cameraPos.xyz);
    float fogStart = cam.fogParams.x;
    float fogEnd   = cam.fogParams.y;
    float fogAmt   = clamp((dist - fogStart) / max(fogEnd - fogStart, 0.001), 0.0, 1.0);

    vec3 fogColor = cam.skyColor.rgb;
    outColor = vec4(mix(lit, fogColor, fogAmt), 1.0);
}

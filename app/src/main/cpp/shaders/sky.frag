#version 450

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

void main() {
    // NDC из gl_FragCoord. Vulkan: y вниз, но проекция уже флипнута.
    vec2 uv  = gl_FragCoord.xy / cam.screenSize.xy;
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    vec4 nearH = cam.invViewProj * vec4(ndc, 0.0, 1.0);
    vec4 farH  = cam.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 nearP = nearH.xyz / nearH.w;
    vec3 farP  = farH.xyz / farH.w;
    vec3 dir = normalize(farP - nearP);

    // Градиент от горизонта к зениту. Базовый цвет приходит из
    // world::DayCycle, поэтому небо само меняется от ночного индиго
    // к рассветному янтарю и дневной лазури.
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 horizon = cam.skyColor.rgb;
    vec3 zenith  = cam.skyColor.rgb * vec3(0.45, 0.62, 1.05);
    vec3 sky = mix(horizon, zenith, pow(t, 0.7));

    // Звёзды проступают, когда небесный свет падает (sunDir.w).
    float night = 1.0 - clamp(cam.sunDir.w, 0.0, 1.0);
    if (night > 0.01 && dir.y > 0.0) {
        vec3 g = floor(dir * 260.0);
        float h = fract(sin(dot(g, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
        float star = step(0.9992, h) * night * dir.y;
        sky += vec3(star);
    }

    // Солнце светит только когда оно над горизонтом.
    float above = clamp(cam.sunDir.y * 4.0, 0.0, 1.0);
    float d   = max(dot(dir, cam.sunDir.xyz), 0.0);
    sky += vec3(1.0, 0.95, 0.80) * pow(d, 256.0) * 2.0 * above;
    sky += vec3(1.0, 0.92, 0.75) * smoothstep(0.9985, 0.9995, d) * 0.6 * above;

    outColor = vec4(sky, 1.0);
}

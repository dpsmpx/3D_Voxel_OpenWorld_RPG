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
    // NDC из gl_FragCoord. В Vulkan начало кадра — левый ВЕРХНИЙ угол,
    // и там же -1 по Y, поэтому перевод прямой, без переворота. Здесь
    // стоял переворот, и небо строилось вверх ногами: зенит внизу,
    // горизонт вверху, солнце зеркально по высоте.
    vec2 uv  = gl_FragCoord.xy / cam.screenSize.xy;
    vec2 ndc = uv * 2.0 - 1.0;

    vec4 nearH = cam.invViewProj * vec4(ndc, 0.0, 1.0);
    vec4 farH  = cam.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 nearP = nearH.xyz / nearH.w;
    vec3 farP  = farH.xyz / farH.w;
    vec3 dir = normalize(farP - nearP);

    float day   = clamp(cam.sunDir.w, 0.0, 1.0);
    float above = smoothstep(-0.10, 0.06, cam.sunDir.y);
    vec3  sunTint = mix(vec3(1.00, 0.52, 0.26), vec3(1.00, 0.97, 0.92),
                        smoothstep(0.0, 0.30, cam.sunDir.y));

    // Градиент от горизонта к зениту. Базовый цвет приходит из
    // world::DayCycle, поэтому небо само меняется от ночного индиго
    // к рассветному янтарю и дневной лазури.
    float up = clamp(dir.y, -1.0, 1.0);
    vec3 horizon = cam.skyColor.rgb;
    vec3 zenith  = cam.skyColor.rgb * vec3(0.42, 0.60, 1.10);
    vec3 sky = mix(horizon, zenith, pow(clamp(up, 0.0, 1.0), 0.55));

    // Под горизонтом — земляная дымка: без неё при взгляде вниз
    // сквозь дальний край мира светит чистое небо, и видно, где он
    // кончается.
    sky = mix(sky, horizon * 0.55, smoothstep(0.0, -0.25, up));

    // Тёплая полоса у самого горизонта в стороне солнца — то, из-за
    // чего рассвет читается как рассвет.
    float toSun = max(dot(normalize(vec3(dir.x, 0.0, dir.z)),
                          normalize(vec3(cam.sunDir.x, 0.0, cam.sunDir.z))), 0.0);
    float band  = exp(-abs(up) * 7.0) * pow(toSun, 3.0);
    sky = mix(sky, sunTint, band * 0.55 * above);

    // Звёзды проступают, когда небесный свет падает (sunDir.w).
    float night = 1.0 - day;
    if (night > 0.01 && up > 0.0) {
        vec3 g = floor(dir * 260.0);
        float h = fract(sin(dot(g, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
        // Неравная яркость: ровная сетка одинаковых точек читается
        // как шум, а не как небо.
        float mag = fract(h * 91.7);
        float star = step(0.9990, h) * night * up * (0.35 + 0.65 * mag);
        sky += vec3(star) * vec3(0.92, 0.95, 1.0);
    }

    // Солнце светит только когда оно над горизонтом.
    float d = max(dot(dir, cam.sunDir.xyz), 0.0);
    sky += sunTint * pow(d, 900.0) * 3.0 * above;                  // диск
    sky += sunTint * pow(d, 48.0) * 0.30 * above;                  // ореол
    sky += sunTint * pow(d, 6.0)  * 0.10 * above * (1.0 - above * 0.5);

    // Луна — напротив солнца, видна ночью.
    float m = max(dot(dir, -cam.sunDir.xyz), 0.0);
    sky += vec3(0.85, 0.88, 1.0) * pow(m, 2400.0) * 2.2 * night;
    sky += vec3(0.55, 0.60, 0.80) * pow(m, 160.0) * 0.10 * night;

    outColor = vec4(sky, 1.0);
}

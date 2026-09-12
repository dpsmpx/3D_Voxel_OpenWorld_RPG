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

// Небо считается в том же линейном пространстве, что и мир: у
// горизонта туман должен переходить в небо незаметно, а он приходит
// оттуда линейным. Разные пространства дали бы видимый стык по
// линии горизонта.
vec3 toLinear(vec3 c) { return c * c; }
vec3 toSrgb(vec3 c)   { return sqrt(max(c, vec3(0.0))); }

vec3 shoulder(vec3 c) {
    float m = max(max(c.r, c.g), c.b);
    if (m <= 0.75) return c;
    float o = m - 0.75;
    return c * ((0.75 + o / (1.0 + o * 2.0)) / m);
}

void main() {
    // NDC из gl_FragCoord. В Vulkan начало кадра — левый ВЕРХНИЙ угол,
    // и там же -1 по Y, поэтому перевод прямой, без переворота.
    vec2 uv  = gl_FragCoord.xy / cam.screenSize.xy;
    vec2 ndc = uv * 2.0 - 1.0;

    vec4 nearH = cam.invViewProj * vec4(ndc, 0.0, 1.0);
    vec4 farH  = cam.invViewProj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(farH.xyz / farH.w - nearH.xyz / nearH.w);

    float day   = clamp(cam.sunDir.w, 0.0, 1.0);
    float night = 1.0 - day;
    float above = smoothstep(-0.10, 0.06, cam.sunDir.y);
    vec3  sunTint = toLinear(mix(vec3(1.00, 0.52, 0.26), vec3(1.00, 0.97, 0.92),
                                 smoothstep(0.0, 0.30, cam.sunDir.y)));

    // Градиент от горизонта к зениту. Базовый цвет приходит из
    // world::DayCycle, поэтому небо само меняется от ночного индиго
    // к рассветному янтарю и дневной лазури.
    float up = clamp(dir.y, -1.0, 1.0);
    vec3 horizon = toLinear(cam.skyColor.rgb);
    vec3 zenith  = horizon * vec3(0.42, 0.60, 1.10);
    vec3 sky = mix(horizon, zenith, pow(clamp(up, 0.0, 1.0), 0.55));

    // Под горизонтом — земляная дымка: без неё при взгляде вниз
    // сквозь дальний край мира светит чистое небо, и видно, где он
    // кончается.
    sky = mix(sky, horizon * 0.55, smoothstep(0.0, -0.25, up));

    // Тёплая полоса у самого горизонта в стороне солнца — то, из-за
    // чего рассвет читается как рассвет.
    float toSun = max(dot(normalize(vec3(dir.x, 0.0, dir.z)),
                          normalize(vec3(cam.sunDir.x, 0.0, cam.sunDir.z))), 0.0);
    sky = mix(sky, sunTint, exp(-abs(up) * 7.0) * pow(toSun, 3.0) * 0.55 * above);

    // Звёзды проступают, когда небесный свет падает.
    if (night > 0.01 && up > 0.0) {
        vec3 g = floor(dir * 260.0);
        float h = fract(sin(dot(g, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
        // Неравная яркость: ровная сетка одинаковых точек читается
        // как шум, а не как небо.
        float mag = fract(h * 91.7);
        float star = step(0.9990, h) * night * up * (0.35 + 0.65 * mag);
        sky += vec3(star) * vec3(0.85, 0.90, 1.0);
    }

    // Солнце светит только когда оно над горизонтом.
    float d = max(dot(dir, cam.sunDir.xyz), 0.0);
    sky += sunTint * pow(d, 900.0) * 3.0 * above;    // диск
    sky += sunTint * pow(d, 48.0)  * 0.30 * above;   // ореол
    sky += sunTint * pow(d, 6.0)   * 0.08 * above;

    // Луна — напротив солнца, видна ночью.
    float m = max(dot(dir, -cam.sunDir.xyz), 0.0);
    sky += vec3(0.72, 0.77, 1.0) * pow(m, 2400.0) * 2.2 * night;
    sky += vec3(0.30, 0.36, 0.64) * pow(m, 160.0) * 0.10 * night;

    outColor = vec4(toSrgb(shoulder(sky)), 1.0);
}

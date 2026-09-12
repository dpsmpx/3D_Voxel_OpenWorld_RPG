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
    vec4 skyColor;
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
    // Та же модель освещения и то же цветовое пространство, что у
    // террейна (voxel.frag). Иначе существа выглядят вырезанными из
    // другой игры: свет у них считался бы по другой формуле, а цвет
    // не проходил бы через линеаризацию.
    vec3 N = normalize(vNormal);

    float day    = clamp(cam.sunDir.w, 0.0, 1.0);
    float above  = smoothstep(-0.10, 0.06, cam.sunDir.y);
    vec3  sunTint = toLinear(mix(vec3(1.00, 0.52, 0.26), vec3(1.00, 0.97, 0.92),
                                 smoothstep(0.0, 0.30, cam.sunDir.y)));
    vec3  skyLin  = toLinear(cam.skyColor.rgb);
    float skyVis  = 0.5 + 0.5 * N.y;

    vec3 ambient = skyLin * (0.16 + 0.34 * skyVis) * (0.25 + 0.75 * day);
    vec3 sun     = sunTint * (max(dot(N, cam.sunDir.xyz), 0.0) * 0.85 * day * above);
    vec3 moon    = vec3(0.04, 0.055, 0.11) * (1.0 - day) * (0.30 + 0.35 * skyVis);

    // Подсветка по краю силуэта: без неё тёмная фигура сливается с
    // тенью, и моба замечаешь только когда он уже бьёт.
    vec3  toEye = cam.cameraPos.xyz - vWorldPos;
    float rim   = pow(1.0 - clamp(dot(N, normalize(toEye)), 0.0, 1.0), 3.0);

    vec3 lit = toLinear(vColor.rgb) * (ambient + sun + moon + 0.02)
             + skyLin * rim * 0.22;
    lit = shoulder(lit);

    vec3  toFrag = vWorldPos - cam.cameraPos.xyz;
    float dist   = length(toFrag);
    float fogAmt = clamp((dist - cam.fogParams.x) /
                         max(cam.fogParams.y - cam.fogParams.x, 0.001), 0.0, 1.0);
    fogAmt = fogAmt * fogAmt * (3.0 - 2.0 * fogAmt);

    float sunAmt   = max(dot(normalize(toFrag), cam.sunDir.xyz), 0.0);
    vec3  fogColor = mix(skyLin, sunTint, pow(sunAmt, 6.0) * 0.45 * above);

    outColor = vec4(toSrgb(mix(lit, fogColor, fogAmt)), vColor.a);
}

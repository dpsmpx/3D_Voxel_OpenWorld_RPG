#version 450

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
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

layout(set = 0, binding = 1) uniform sampler2D atlas;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(atlas, vUv);
    if (tex.a < 0.4) discard;

    // Та же модель, что у террейна и мобов. Билборд смотрит на камеру,
    // поэтому своей нормали у него нет: считаем, что трава открыта небу
    // почти полностью, а солнце подсвечивает её на просвет.
    float day    = clamp(cam.sunDir.w, 0.0, 1.0);
    vec3  sunTint = mix(vec3(1.00, 0.52, 0.26), vec3(1.00, 0.97, 0.92),
                        smoothstep(0.0, 0.30, cam.sunDir.y));
    float above  = smoothstep(-0.10, 0.06, cam.sunDir.y);
    const float skyVis = 0.85;

    vec3 ambient = cam.skyColor.rgb * (0.12 + 0.26 * skyVis) * (0.25 + 0.75 * day);
    vec3 sun     = sunTint * (0.52 * day * above);
    vec3 moon    = vec3(0.20, 0.24, 0.36) * (1.0 - day) * (0.30 + 0.35 * skyVis);

    vec3 lit = tex.rgb * vColor.rgb * (ambient + sun + moon + 0.04);

    vec3  toFrag = vWorldPos - cam.cameraPos.xyz;
    float dist   = length(toFrag);
    float fogAmt = clamp((dist - cam.fogParams.x) /
                         max(cam.fogParams.y - cam.fogParams.x, 0.001), 0.0, 1.0);
    fogAmt = fogAmt * fogAmt * (3.0 - 2.0 * fogAmt);

    float sunAmt   = max(dot(normalize(toFrag), cam.sunDir.xyz), 0.0);
    vec3  fogColor = mix(cam.skyColor.rgb, sunTint,
                         pow(sunAmt, 6.0) * 0.45 * above);

    outColor = vec4(mix(lit, fogColor, fogAmt), tex.a * vColor.a);
}

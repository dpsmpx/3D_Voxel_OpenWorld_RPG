#version 450

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;
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

    // Градиент
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 horizon = vec3(0.72, 0.85, 0.95);
    vec3 zenith  = vec3(0.18, 0.42, 0.78);
    vec3 sky = mix(horizon, zenith, pow(t, 0.7));

    // Солнце
    float sun = pow(max(dot(dir, cam.sunDir.xyz), 0.0), 256.0);
    sky += vec3(1.0, 0.95, 0.80) * sun * 2.0;

    // Лёгкий диск солнца — мягкий
    float disc = smoothstep(0.9985, 0.9995, dot(dir, cam.sunDir.xyz));
    sky += vec3(1.0, 0.92, 0.75) * disc * 0.6;

    outColor = vec4(sky, 1.0);
}

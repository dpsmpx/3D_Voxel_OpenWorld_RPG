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
    vec4 fogParams;   // start, end, density, time
} cam;

layout(set = 0, binding = 1) uniform sampler2D atlas;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(atlas, vUv);
    if (tex.a < 0.1) discard;

    vec3 lit = tex.rgb * vColor.rgb;

    // Атмосферный туман
    float dist = distance(vWorldPos, cam.cameraPos.xyz);
    float fogStart = cam.fogParams.x;
    float fogEnd   = cam.fogParams.y;
    float fogAmt   = clamp((dist - fogStart) / max(fogEnd - fogStart, 0.001), 0.0, 1.0);

    vec3 fogColor = vec3(0.55, 0.72, 0.92);
    outColor = vec4(mix(lit, fogColor, fogAmt), tex.a);
}
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
} cam;

layout(location = 0) out vec4 outColor;

void main() {
    float ndl = max(dot(vNormal, cam.sunDir.xyz), 0.0);
    float light = 0.35 + 0.75 * ndl;
    vec3 lit = vColor.rgb * light;

    float dist = distance(vWorldPos, cam.cameraPos.xyz);
    float fogAmt = clamp((dist - cam.fogParams.x) / max(cam.fogParams.y - cam.fogParams.x, 0.001), 0.0, 1.0);
    vec3 fogColor = vec3(0.55, 0.72, 0.92);

    outColor = vec4(mix(lit, fogColor, fogAmt), vColor.a);
}

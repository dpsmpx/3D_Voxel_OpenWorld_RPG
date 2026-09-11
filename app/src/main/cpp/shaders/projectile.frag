#version 450

layout(location = 0) in vec4 vColor;

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
    outColor = vec4(vColor.rgb * 1.4, vColor.a);
}

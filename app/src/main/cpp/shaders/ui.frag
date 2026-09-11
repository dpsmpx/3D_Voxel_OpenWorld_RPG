#version 450
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 t = texture(atlas, vUv);
    outColor = t * vColor;
}

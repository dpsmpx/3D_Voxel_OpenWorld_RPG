#version 450
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;

layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) out vec4 outColor;

void main() {
    // Отрицательная UV значит «без текстуры»: сплошная заливка.
    // Раньше и она шла через атлас — в белый тексель, — и любая беда
    // с атласом, сэмплером или набором дескрипторов делала весь
    // интерфейс невидимым, ничем себя не выдав. Теперь от текстуры
    // зависят только буквы и миникарта, а заодно это на одну выборку
    // меньше для большей части интерфейса.
    vec4 t = vUv.x < 0.0 ? vec4(1.0) : texture(atlas, vUv);
    outColor = t * vColor;
}

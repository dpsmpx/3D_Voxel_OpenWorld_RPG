#version 450
// Мобильный GPU: mediump хватает цвету и освещению, highp оставляем
// мировым координатам — по ним считается и клетка вокселя, и туман.
precision mediump float;
precision mediump int;

layout(location = 0) in       vec4  vColor;
layout(location = 1) in highp vec3  vWorldPos;
layout(location = 2) in       vec2  vShade;
layout(location = 3) in flat  uint  vInfo;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 cameraPos;
    vec4 screenSize;
    vec4 sunDir;
    vec4 fogParams;   // start, end, timeOfDay, time
    vec4 skyColor;
} cam;

layout(location = 0) out vec4 outColor;

// Нормали граней в порядке world::FACES: +X, -X, +Y, -Y, +Z, -Z.
const vec3 FACE_N[6] = vec3[6](
    vec3( 1.0,  0.0,  0.0), vec3(-1.0,  0.0,  0.0),
    vec3( 0.0,  1.0,  0.0), vec3( 0.0, -1.0,  0.0),
    vec3( 0.0,  0.0,  1.0), vec3( 0.0,  0.0, -1.0));

// Свет считаем в линейном пространстве, а материалы и небо заданы в
// sRGB. Без этого перевода умножение на освещённость съедает
// полутона, и мир выглядит грязным. Приближение гаммой 2.0 вместо
// 2.2 стоит одного умножения и одного корня.
vec3 toLinear(vec3 c) { return c * c; }
vec3 toSrgb(vec3 c)   { return sqrt(max(c, vec3(0.0))); }

// Плечо только для пересветов: до 0.75 не трогаем ничего, выше —
// мягко подводим к единице по самому яркому каналу, чтобы солнце на
// снегу не выбивало цвет в белое пятно.
vec3 shoulder(vec3 c) {
    float m = max(max(c.r, c.g), c.b);
    if (m <= 0.75) return c;
    float o = m - 0.75;
    return c * ((0.75 + o / (1.0 + o * 2.0)) / m);
}

highp float hash13(highp vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

void main() {
    int  face  = int(vInfo & 7u);
    vec3 N     = FACE_N[face];
    float grain = float((vInfo >> 3) & 7u) * (1.0 / 7.0) * 0.55;
    bool  tint  = ((vInfo >> 6) & 1u) != 0u;

    // ---- крапчатость по клеткам ----
    // Текстур нет, и ровная заливка читается как пластик. Шум с шагом
    // ровно в один воксель возвращает поверхности материальность и,
    // что важнее, делает видимой саму воксельную сетку — даже там,
    // где жадное слияние собрало полсотни граней в один квад.
    highp vec3 cell = floor(vWorldPos - N * 0.5);
    float n = hash13(cell) - 0.5;

    // ---- фаска по краям вокселя ----
    // Тонкое затемнение у рёбер клетки: объём читается сразу, куб
    // перестаёт быть плоским пятном. Пропадает, когда воксель мельче
    // пикселя, иначе на дальних склонах пошла бы рябь.
    highp vec2 fp = abs(N.x) > 0.5 ? vWorldPos.zy
                  : (abs(N.y) > 0.5 ? vWorldPos.xz : vWorldPos.xy);
    highp vec2 f  = abs(fract(fp) - 0.5);
    highp float d = 0.5 - max(f.x, f.y);
    float w = fwidth(d) * 1.5 + 1e-4;
    float bevel = mix(1.0, mix(0.84, 1.0, smoothstep(0.0, max(w, 0.02), d)),
                      clamp(1.0 - w * 2.5, 0.0, 1.0));

    vec3 albedo = toLinear(vColor.rgb) * (1.0 + grain * n) * bevel;

    // Подкраска по местности. Не настоящие биомы — те живут в
    // генераторе и менялись бы от колонки к колонке, а это разорвало
    // бы жадное слияние граней подчистую. Здесь достаточно медленной
    // волны по мировым координатам: она даёт переход от прохладной
    // зелени к тёплой оливковой, а поляна перестаёт выглядеть
    // выкрашенной одной банкой.
    if (tint) {
        highp float t = sin(vWorldPos.x * 0.021 + sin(vWorldPos.z * 0.013) * 2.3)
                      * 0.5 + 0.5;
        albedo *= mix(vec3(0.82, 1.05, 0.78), vec3(1.14, 0.95, 0.66), t);
    }

    // Высота. Внизу холоднее и темнее, на вершинах светлее и теплее:
    // дешёвый способ разложить рельеф по планам, когда весь цвет
    // держится на материале.
    float hf = clamp((vWorldPos.y - 40.0) / 70.0, 0.0, 1.0);
    albedo *= mix(vec3(0.90, 0.93, 1.00), vec3(1.05, 1.03, 0.97), hf);

    // ---- освещение ----
    vec3  skyLin  = toLinear(cam.skyColor.rgb);
    float day     = clamp(cam.sunDir.w, 0.0, 1.0);
    float above   = smoothstep(-0.10, 0.06, cam.sunDir.y);
    vec3  sunTint = toLinear(mix(vec3(1.00, 0.52, 0.26), vec3(1.00, 0.97, 0.92),
                                 smoothstep(0.0, 0.30, cam.sunDir.y)));

    // Затенение углов запечено в геометрию мешером. Небо им гасится
    // полностью — оно приходит со всех сторон и в щель не попадает;
    // прямое солнце гасится частично, иначе освещённые склоны теряют
    // контраст и мир становится плоским.
    float ao    = 0.42 + 0.58 * vShade.x;
    float aoSun = mix(1.0, ao, 0.6);

    // Открытость неба. Под сводом пещеры небо не светит вовсе, а
    // солнце тем более: без этого пещера освещена так же, как
    // открытый склон. Остаток не нулевой — в полной темноте пещера не
    // читается, она просто исчезает.
    float sky    = 0.18 + 0.82 * vShade.y;
    float skySun = smoothstep(0.35, 0.85, vShade.y);

    float skyVis = 0.5 + 0.5 * N.y;
    float ndl    = max(dot(N, cam.sunDir.xyz), 0.0);

    vec3 ambient = skyLin * (0.16 + 0.34 * skyVis) * (0.25 + 0.75 * day) * ao * sky;
    vec3 sun      = sunTint * (ndl * 0.85 * day * above) * aoSun * skySun;
    vec3 moon    = vec3(0.04, 0.055, 0.11) * (1.0 - day) * (0.30 + 0.35 * skyVis) * ao * sky;

    vec3 lit = albedo * (ambient + sun + moon + 0.02);

    // Блик на полупрозрачном. Вода и лёд отличаются от камня не
    // цветом, а тем, что отражают небо: без блика вода читается как
    // синее стекло, положенное на дно.
    if (vColor.a < 0.99) {
        highp vec3 V = normalize(cam.cameraPos.xyz - vWorldPos);
        vec3 H = normalize(V + cam.sunDir.xyz);
        float spec = pow(max(dot(N, H), 0.0), 64.0) * day * above;
        // Скользящий взгляд отражает сильнее — приближение Френеля.
        float fres = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 4.0);
        lit += sunTint * spec * 0.9;
        lit += skyLin * fres * 0.35;
    }

    lit = shoulder(lit);

    // ---- туман ----
    highp vec3 toFrag = vWorldPos - cam.cameraPos.xyz;
    float dist   = length(toFrag);
    float fogAmt = clamp((dist - cam.fogParams.x) /
                         max(cam.fogParams.y - cam.fogParams.x, 0.001), 0.0, 1.0);
    fogAmt = fogAmt * fogAmt * (3.0 - 2.0 * fogAmt);

    float sunAmt   = max(dot(normalize(toFrag), cam.sunDir.xyz), 0.0);
    vec3  fogColor = mix(skyLin, sunTint, pow(sunAmt, 6.0) * 0.45 * above);

    outColor = vec4(toSrgb(mix(lit, fogColor, fogAmt)), vColor.a);
}

#version 450
// Мобильный GPU: mediump хватает цвету и освещению, highp оставляем
// мировым координатам — по ним считается и клетка вокселя, и туман.
precision mediump float;
precision mediump int;

// Точность объявлена ЯВНО и совпадает с вершинным шейдером.
//
// Вершинный шейдер не объявляет precision вовсе, то есть весь он
// highp; фрагментный начинается с precision mediump, и три из четырёх
// межстадийных переменных получали RelaxedPrecision только с одной
// стороны. Стыковка интерфейсов сверяет тип и расположение, точность —
// нет, поэтому расхождение проходило молча, а хранил и передавал такую
// переменную драйвер по своему усмотрению.
//
// Больнее всего это било по vShade: из неё считаются множители
// освещения, и сверху они ничем не ограничены.
layout(location = 0) in highp vec4  vColor;
layout(location = 1) in highp vec3  vWorldPos;
layout(location = 2) in highp vec2  vShade;
layout(location = 3) in flat highp uint vInfo;

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

// Отладочные виды: одно слагаемое вместо всей картинки.
//
// Артефакт, который видно на устройстве, может не повторяться ни в
// офлайн-рендере, ни в программном Vulkan — там другой драйвер и
// другая точность. Тогда единственный способ узнать, какой из входов
// фрагмента врёт, — показать их по одному прямо на устройстве.
// Номер вида приходит в свободной компоненте screenSize.z, так что ни
// второго конвейера, ни отдельных .spv не нужно.
const vec3 FACE_DBG[6] = vec3[6](
    vec3(1.0, 0.2, 0.2), vec3(0.6, 0.1, 0.1),
    vec3(0.2, 1.0, 0.2), vec3(0.1, 0.5, 0.1),
    vec3(0.2, 0.4, 1.0), vec3(0.1, 0.2, 0.6));

bool debugView(int mode, int face, out vec4 outc) {
    outc = vec4(0.0, 0.0, 0.0, 1.0);
    if (mode == 1) outc = vec4(vColor.rgb, 1.0);                 // цвет вершины
    else if (mode == 2) outc = vec4(vec3(vShade.y), 1.0);        // открытость неба
    else if (mode == 3) outc = vec4(vec3(vShade.x), 1.0);        // затенение углов
    else if (mode == 4) outc = vec4(FACE_DBG[face], 1.0);        // номер грани
    else if (mode == 5) {                                        // расстояние
        highp float d = length(vWorldPos - cam.cameraPos.xyz);
        outc = vec4(vec3(clamp(d / 256.0, 0.0, 1.0)), 1.0);
    }
    else if (mode == 6) outc = vec4(vec3(clamp(vWorldPos.y / 128.0, 0.0, 1.0)), 1.0);
    else return false;
    return true;
}

void main() {
    int  face  = int(vInfo & 7u);

    {
        vec4 dbg;
        if (debugView(int(cam.screenSize.z + 0.5), face, dbg)) {
            outColor = dbg;
            return;
        }
    }
    vec3 N     = FACE_N[face];
    bool  tint  = ((vInfo >> 6) & 1u) != 0u;

    // Затенение углов и открытость неба приходят долями [0,1] по
    // построению: мешер кладёт в биты значения 0..3 и 0..7, вершинный
    // шейдер делит. Ограничение здесь не косметика, а защита
    // контракта: множители ниже ничем не ограничены сверху, и любое
    // значение вне диапазона уводит освещение либо в пересвет, либо в
    // чёрное — ровно это и было видно на устройстве.
    highp vec2 shade = clamp(vShade, 0.0, 1.0);

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

    // Цвет грани — цвет материала, и больше ничего. Здесь стояла
    // крапчатость по клеткам: hash13 от номера вокселя давал каждой
    // клетке свой оттенок. Ровной поверхности она не давала стать
    // ровной, а на дальних гранях, где клетка мельче пикселя,
    // рассыпалась в мерцающую пыль.
    vec3 albedo = toLinear(vColor.rgb) * bevel;

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

    // Небо как источник: его цвет, но приглушённый к белому. Умножать
    // на сам skyLin нельзя — он в линейном пространстве тёмный, и всё
    // в тени уходило в чёрно-синее, а материал переставал читаться.
    float skyMax = max(max(skyLin.r, skyLin.g), max(skyLin.b, 0.001));
    vec3  skyTint = mix(vec3(1.0), skyLin / skyMax, 0.60);
    // Снизу светит не небо, а отражение земли — тёплое и слабое.
    const vec3 groundTint = vec3(0.62, 0.56, 0.46);

    // Затенение углов запечено в геометрию мешером. Небо им гасится
    // полностью — оно приходит со всех сторон и в щель не попадает;
    // прямое солнце гасится частично, иначе освещённые склоны теряют
    // контраст и мир становится плоским.
    float ao    = 0.42 + 0.58 * shade.x;
    float aoSun = mix(1.0, ao, 0.6);

    // Открытость неба. Под сводом пещеры небо не светит вовсе, а
    // солнце тем более: без этого пещера освещена так же, как
    // открытый склон. Остаток не нулевой — в полной темноте пещера не
    // читается, она просто исчезает.
    float sky    = 0.18 + 0.82 * shade.y;
    float skySun = smoothstep(0.35, 0.85, shade.y);

    float skyVis = 0.5 + 0.5 * N.y;
    float ndl    = max(dot(N, cam.sunDir.xyz), 0.0);

    // Полусфера: сверху небо, снизу отражение земли. Освещённость
    // подобрана так, чтобы грань в тени была примерно вдвое темнее
    // освещённой, а не в двадцать раз: физически честное отношение
    // прямого света к небесному превращает боковые грани в чёрные
    // дыры, и мир перестаёт читаться как объём.
    vec3  hemi = mix(groundTint, skyTint, skyVis);
    float amb  = mix(0.34, 0.66, skyVis) * (0.30 + 0.70 * day);

    vec3 ambient = hemi * amb * ao * sky;
    vec3 sun     = sunTint * (ndl * 0.52 * day * above) * aoSun * skySun;
    vec3 moon    = vec3(0.05, 0.065, 0.12) * (1.0 - day) * (0.35 + 0.35 * skyVis) * ao * sky;

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
    vec3  fogColor = mix(skyLin, sunTint, pow(sunAmt, 8.0) * 0.30 * above);

    outColor = vec4(toSrgb(mix(lit, fogColor, fogAmt)), vColor.a);
}

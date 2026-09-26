/**
 * @file landform.cpp
 * @brief Мир: крупные формы рельефа — из чего складывается высота.
 */
#include "landform.h"
#include <algorithm>
#include <cmath>

namespace world {

namespace {

inline f32 smoothstep(f32 a, f32 b, f32 x) {
    const f32 t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

/// Уступы: t в 0..1 превращается в ступени с крутыми подъёмами.
/// Не floor по линейке — между ступенями склон, а не отвесная стена
/// в один воксель: крупные блоки и так дают обрыв.
inline f32 terrace(f32 t, f32 steps) {
    const f32 s = t * steps;
    const f32 k = std::floor(s);
    const f32 f = s - k;
    return (k + smoothstep(0.55f, 0.95f, f)) / steps;
}

} // namespace

const char* landformName(Landform f) {
    switch (f) {
        case Landform::Ocean:     return "ocean";
        case Landform::Coast:     return "coast";
        case Landform::Lowland:   return "lowland";
        case Landform::Plains:    return "plains";
        case Landform::Hills:     return "hills";
        case Landform::Plateau:   return "plateau";
        case Landform::Foothills: return "foothills";
        case Landform::Mountains: return "mountains";
        case Landform::Peaks:     return "peaks";
        default:                  return "?";
    }
}

u32 landformDebugColor(Landform f) {
    switch (f) {
        case Landform::Ocean:     return 0x2E5A9Cu;
        case Landform::Coast:     return 0xE8D8A0u;
        case Landform::Lowland:   return 0x5A8A6Au;
        case Landform::Plains:    return 0xA8CC78u;
        case Landform::Hills:     return 0x6FA050u;
        case Landform::Plateau:   return 0xC89A5Au;
        case Landform::Foothills: return 0x9A9A70u;
        case Landform::Mountains: return 0x7A7A7Au;
        case Landform::Peaks:     return 0xEEEEF4u;
        default:                  return 0xFF00FFu;
    }
}

OrientFrames OrientFrames::of(f32 angle) {
    // Вытянутый узор π-периодичен: хребет с севера на юг и с юга на
    // север — один и тот же хребет, поэтому 2·(угол − ось). Окно каждой
    // оси — ±60°, и соседние оси перекрываются ровно наполовину.
    OrientFrames f;
    f32 sum = 0.f, sq = 0.f;
    for (i32 k = 0; k < 3; ++k) {
        const f32 th = (f32)k * 1.0471976f;
        f32 w = std::cos(2.f * (angle - th)) + 0.5f;
        w = w > 0.f ? w * w : 0.f;
        f.w[k] = w;
        sum += w;
    }
    for (i32 k = 0; k < 3; ++k) { f.w[k] /= sum; sq += f.w[k] * f.w[k]; }
    f.norm = 1.f / std::sqrt(sq);
    return f;
}

LandformNoise::LandformNoise(u64 seed)
    : mountain_(seed ^ 0x4D0A7A1Eull),
      warp_(seed ^ 0x3A29E5ull),
      hills_(seed ^ 0x81115ull),
      plateau_(seed ^ 0x9A7E4Bull),
      detail_(seed ^ 0xDE7A11ull)
{}

f32 LandformNoise::ridgedMultifractal(f32 x, f32 z, u32 octaves) const {
    // Масгрейв: каждая следующая октава взвешена предыдущим сигналом.
    // Где сигнал высок (гребень), мелкие детали добавляются в полную
    // силу — гребень рваный; где низок (распадок), они гаснут —
    // дно долины гладкое, как намытое. Обычный fBm так не умеет: у
    // него детали всюду одинаковы, и горы выходят «мятой бумагой».
    constexpr f32 OFFSET = 1.0f;
    constexpr f32 GAIN   = 2.0f;
    constexpr f32 LAC    = 2.1f;
    f32 signal = OFFSET - std::fabs(mountain_.sample2D(x, z));
    signal *= signal;
    f32 result = signal, weight = 1.f, freq = 1.f, amp = 1.f, norm = 1.f;
    for (u32 o = 1; o < octaves; ++o) {
        freq *= LAC;
        amp  *= 0.5f;
        weight = std::clamp(signal * GAIN, 0.f, 1.f);
        signal = OFFSET - std::fabs(mountain_.sample2D(x * freq + 13.1f * o,
                                                       z * freq - 7.7f * o));
        signal *= signal * weight;
        result += signal * amp;
        norm   += amp;
    }
    return result / norm;
}

LandformSample LandformNoise::sample(const MacroFields& m, i32 x, i32 z) const {
    const f32 fx = (f32)x, fz = (f32)z;
    LandformSample out;

    // ---- macro: суша и море ----
    //
    // Та же кривая континента, что и раньше: доля моря и высота
    // равнин от неё зависят, и гидросеть на неё настроена.
    const f32 C = m.continent;
    f32 h = 33.f + (C > 0.f ? C * 45.f : C * 70.f);

    const f32 M = m.mountain;
    const f32 land = smoothstep(-0.02f, 0.10f, C);

    f32 route = h;   // крупный рельеф без холмов — см. LandformSample::route

    // ---- горная страна ----
    //
    // Оси области: хребты тянутся вдоль u. По u частота ниже, чем по
    // v, поэтому гребни вытянуты — цепь, а не россыпь куполов.
    // Искажение сдвигает точку выборки, и гребни изгибаются, а не
    // идут по линейке.
    f32 ridge = 0.f;
    if (M > 0.002f) {
        const f32 wx = warp_.fbm3D(fx * 0.0018f, 0.f, fz * 0.0018f, 2) * 110.f;
        const f32 wz = warp_.fbm3D(fx * 0.0018f + 31.7f, 0.f, fz * 0.0018f - 12.3f, 2) * 110.f;
        const OrientFrames of = OrientFrames::of(m.orient);
        f32 mf = 0.f;
        for (i32 k = 0; k < 3; ++k) {
            if (of.w[k] <= 0.f) continue;
            const f32 u =  (fx + wx) * OrientFrames::COS[k] + (fz + wz) * OrientFrames::SIN[k];
            const f32 v = -(fx + wx) * OrientFrames::SIN[k] + (fz + wz) * OrientFrames::COS[k];
            mf += of.w[k] * ridgedMultifractal(u * 0.0024f + 17.f * k, v * 0.0062f, 5);
        }
        ridge = std::clamp(mf, 0.f, 1.f);

        // Высота хребта своя у каждого участка гор: одна и та же
        // амплитуда всюду дала бы горы одной высоты, как забор.
        const f32 amp = 34.f + 34.f * m.peaks + 12.f * m.region;
        const f32 core = std::pow(M, 1.25f);
        // Мультифрактал нормирован суммой октав, и его размах — около
        // половины единицы: гребни выходили в два десятка блоков над
        // распадками, и горная страна читалась плоским каменным полем.
        // Растягиваем на полный размах; дно распадков чуть ниже
        // подножия, гребни — на всю амплитуду.
        const f32 relief = std::clamp(mf * 2.1f - 0.25f, -0.1f, 1.4f);
        // Хребет стоит на поднятии: середина горной страны выше её
        // края, и вершины собираются к оси, а не рассыпаны поровну.
        h += core * amp * relief + M * M * 16.f;
        // Сток видит гребни вполовину. Гребни вытянуты вдоль оси
        // хребта, и распадок между двумя из них — корыто, закрытое с
        // концов: по полному рельефу в каждом стояло длинное озеро, и
        // горная страна выходила полосатой от воды. Вполовину — вода
        // переваливает седловины и уходит с гор поперёк хребтов, а
        // долина врезается там, где она прошла.
        route += core * amp * relief * 0.5f + M * M * 16.f;
    }

    // ---- холмы ----
    //
    // Холмы — «клубы» (|шум|), а не синусоида. Разница не в облике, а
    // в воде: у синусоиды низины — отдельные ямы между буграми, и
    // каждая река на холмах стояла цепью озёр. У клубов низина — сеть
    // узких ложбин по линиям смены знака шума, связная, и вода по ней
    // уходит, как по настоящим распадкам. Сила холмов — от характера
    // области.
    const f32 H = m.hills * (1.f - M) * land;
    if (H > 0.002f) {
        const f32 wx = warp_.sample2D(fx * 0.004f - 5.1f, fz * 0.004f + 2.3f) * 40.f;
        const f32 wz = warp_.sample2D(fx * 0.004f + 9.7f, fz * 0.004f - 4.4f) * 40.f;
        const f32 n = hills_.fbm3D((fx + wx) * 0.0075f, 0.f, (fz + wz) * 0.0075f, 3);
        f32 s = std::clamp(std::fabs(n) * 2.2f, 0.f, 1.f);
        s = std::sqrt(s);   // скруглённый верх, узкая ложбина
        const f32 amp = (11.f + 13.f * m.region) * (1.2f - 0.5f * m.erosion);
        // Гривки: невысокие вытянутые гряды между холмами — ложбины
        // получают направление, и склоны перестают быть блинами.
        const f32 g = 1.f - std::fabs(hills_.sample2D((fx - wz) * 0.016f, (fz + wx) * 0.016f));
        h += H * amp * (s * 1.25f - 0.35f) + H * amp * 0.25f * g * g;
    }

    // ---- плато ----
    //
    // Ровный верх и обрывистый край уступами. Маска уже с резким краем
    // (BiomeField), здесь из неё делаются ступени.
    const f32 P = m.plateau * (1.f - M) * land;
    if (P > 0.002f) {
        const f32 amp = 13.f + 13.f * m.region;
        const f32 stepped = terrace(P, 3.f);
        const f32 top = 1.2f * detail_.sample2D(fx * 0.02f, fz * 0.02f);
        h += amp * (P * 0.3f + stepped * 0.7f) + P * top;
        route += amp * (P * 0.3f + stepped * 0.7f);
    }

    // ---- низины и равнины ----
    h -= m.basin * 2.5f * land;
    route -= m.basin * 2.5f * land;
    const f32 flat = (1.f - H) * (1.f - M) * (1.f - P);
    // Равнина — не стол: пологие увалы в сотню блоков длиной.
    h += flat * 3.5f * detail_.fbm3D(fx * 0.0060f, 0.f, fz * 0.0060f, 2);

    // ---- micro ----
    //
    // Лёгкая неровность. Сильнее на изрезанных участках, слабее в
    // низинах: болото ровное.
    const f32 rough = 0.5f + 1.2f * (1.f - m.erosion);
    h += detail_.sample2D(fx * 0.045f, fz * 0.045f) * rough * (1.f - 0.7f * m.basin);

    out.height = h;
    out.route = route;
    out.ridge = ridge * M;

    // ---- характер местности ----
    if (h < 24.f && C < 0.05f)          out.form = Landform::Ocean;
    else if (h < 28.f && C < 0.15f)     out.form = Landform::Coast;
    else if (M > 0.55f)                 out.form = h > 88.f ? Landform::Peaks : Landform::Mountains;
    else if (M > 0.18f)                 out.form = Landform::Foothills;
    else if (P > 0.5f)                  out.form = Landform::Plateau;
    else if (m.basin > 0.5f)            out.form = Landform::Lowland;
    else if (H > 0.45f)                 out.form = Landform::Hills;
    else                                out.form = Landform::Plains;
    return out;
}

} // namespace world

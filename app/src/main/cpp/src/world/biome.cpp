/**
 * @file biome.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "biome.h"
#include "noise.h"
#include "block.h"
#include <cmath>
#include <array>
#include <algorithm>
#include <memory>

namespace world {

// ============================================================
// Таблица биомов. Значения подобраны чтобы давать разнообразный
// но не шизофреничный мир.
// ============================================================
static const std::array<BiomeDef, BIOME_COUNT> BIOMES = {{
    // Ocean
    { "Ocean",    SAND,    DIRT,    STONE, WATER, 0.00f, TreeType::None, 0,  -0.3f },
    // Beach
    { "Beach",    SAND,    SAND,    STONE, WATER, 0.30f, TreeType::Palm, 0,   0.3f },
    // Plains
    { "Plains",   GRASS,   DIRT,    STONE, WATER, 1.40f, TreeType::Oak, 2,   0.2f },
    // Forest
    { "Forest",   GRASS,   DIRT,    STONE, WATER, 7.00f, TreeType::Oak, 3,   0.1f },
    // Taiga
    { "Taiga",    GRASS,   DIRT,    STONE, WATER, 5.00f, TreeType::Pine, 4,  -0.5f },
    // Desert
    { "Desert",   SAND,    SAND,    STONE, WATER, 0.25f, TreeType::Cactus, 0,   0.6f },
    // Savanna
    { "Savanna",  GRASS,   DIRT,    STONE, WATER, 1.20f, TreeType::Dead, 1,   0.5f },
    // Tundra
    { "Tundra",   SNOW,    DIRT,    STONE, ICE,   0.40f, TreeType::Pine, 3,  -0.8f },
    // Mountains
    { "Mountains",STONE,   STONE,   STONE, WATER, 0.60f, TreeType::Pine, 20, -0.4f },
    // Swamp
    { "Swamp",    GRASS,   DIRT,    STONE, WATER, 2.50f, TreeType::Dead, -2,  0.4f },
    // Volcanic
    { "Volcanic", STONE,   STONE,   STONE, LAVA,  0.00f, TreeType::None, 15,  0.8f },
    // Blight — Чёрный лес.
    //
    // Плотность 14 — вдвое против обычного леса: сквозь такой лес не
    // видно, и это главное, что делает место зловещим. Поверхность
    // голая земля, а не трава: в Чёрном лесу ничего не растёт, кроме
    // самого леса.
    { "Blight",   DIRT,    DIRT,    STONE, WATER, 14.00f, TreeType::Dead, 1,  0.0f },
}};

/// Климат, к которому подтягивается околица. Всё безопасное.
///
/// Температура и влажность выбраны так, чтобы classify() дал нужный
/// биом даже с учётом оставшейся доли собственного шума мира: запас
/// до порога соседнего биома больше, чем эта доля может сдвинуть.
/// Проверка перебирает зёрна и убеждается в этом числами.
struct HomeClimate { f32 temperature, humidity; };
static constexpr HomeClimate HOME_CLIMATE[(u32)HomeKind::Count] = {
    { 0.20f,  0.00f },   // Plains  — чистое поле
    { 0.15f,  0.20f },   // Forest  — густой лес
    { 0.35f, -0.18f },   // Savanna — редколесье
    {-0.20f,  0.20f },   // Taiga   — сосняк
};

f32 homeWeight(f32 d) {
    if (d >= HOME_FADE_RADIUS) return 0.f;
    if (d <= HOME_FULL_RADIUS) return HOME_WEIGHT;
    const f32 t = (d - HOME_FULL_RADIUS)
                / (HOME_FADE_RADIUS - HOME_FULL_RADIUS);
    // smoothstep: у подтяжки не должно быть ни ступеньки, ни излома —
    // иначе околица видна кольцом на рельефе.
    return HOME_WEIGHT * (1.f - t * t * (3.f - 2.f * t));
}

struct BiomeField::Impl {
    SimplexNoise continent;
    SimplexNoise temperature;
    SimplexNoise humidity;
    SimplexNoise erosion;
    SimplexNoise peaks;
    SimplexNoise weird;
    SimplexNoise uplift;

    /// Какая околица досталась миру. Выбирается зерном один раз.
    HomeKind home = HomeKind::Plains;

    explicit Impl(u64 seed)
        : continent(seed ^ 0x1111),
          temperature(seed ^ 0x2222),
          humidity(seed ^ 0x3333),
          erosion(seed ^ 0x4444),
          peaks(seed ^ 0x5555),
          weird(seed ^ 0x6666),
          uplift(seed ^ 0x7777)
    {
        // Перемешивание зерна перед выбором: младшие биты у соседних
        // зёрен отличаются на единицу, и без перемешивания миры 1, 2,
        // 3, 4 получили бы все четыре околицы по кругу — «случайно»
        // ровно до первого взгляда на список.
        u64 h = seed ^ 0xA24BAED4963EE407ULL;
        h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 32; h *= 0x94D049BB133111EBULL;
        h ^= h >> 31;
        home = (HomeKind)(h % (u64)HomeKind::Count);
    }
};

BiomeField::BiomeField(u64 seed) : impl_(std::make_unique<Impl>(seed)) {}
BiomeField::~BiomeField() = default;

HomeKind BiomeField::home() const { return impl_->home; }
// (в реальном проекте — unique_ptr, но здесь упрощаем для краткости)

BiomeField::Sample BiomeField::fields(i32 x, i32 z) const {
    const f32 fx = (f32)x;
    const f32 fz = (f32)z;

    Sample s{};
    // Континенты — очень низкочастотный шум
    s.continent = impl_->continent.fbm3D(fx * 0.0004f, 0.f, fz * 0.0004f, 4, 2.f, 0.5f);
    // Температура — среднечастотная
    s.temperature = impl_->temperature.fbm3D(fx * 0.0012f, 0.f, fz * 0.0012f, 3);
    // Влажность
    s.humidity = impl_->humidity.fbm3D(fx * 0.0010f, 0.f, fz * 0.0010f, 3);
    // Эрозия — 0 = горы, 1 = равнина
    s.erosion = impl_->erosion.fbm3D(fx * 0.0020f, 0.f, fz * 0.0020f, 2) * 0.5f + 0.5f;
    // Пики
    s.peaks = std::max(0.f, impl_->peaks.fbm3D(fx * 0.0030f, 0.f, fz * 0.0030f, 3));
    // Порча. Частота между континентами и температурой: пятна
    // Чёрного леса должны быть больше деревни и меньше материка —
    // иначе это либо рощица, либо полмира.
    s.weird = impl_->weird.fbm3D(fx * 0.0008f, 0.f, fz * 0.0008f, 3);

    // Хребты. Частота ниже температуры и выше континентов: горная
    // цепь должна быть длиннее области, и короче материка.
    //
    // Гребень, а не купол: |шум| даёт ноль по линии смены знака, и
    // 1 - |шум| поднимает как раз эту линию — получается ХРЕБЕТ, а
    // не круглый холм. Тем же приёмом рисует скалы terrain.cpp.
    {
        const f32 raw = impl_->uplift.fbm3D(fx * 0.00055f, 0.f, fz * 0.00055f, 3);

        // Узкая полоса вокруг НУЛЯ шума, а не «единица минус модуль».
        // Второе кажется тем же самым, но у fBm значения жмутся к
        // нулю: модуль в среднем 0.23, и «чуть выше нуля» — это
        // больше половины карты. Мир вышел горами на пятьдесят три
        // процента. Полоса в полсотых — четырнадцать процентов
        // площади, и это уже хребты, а не нагорье.
        constexpr f32 RIDGE_HALF = 0.055f;
        s.uplift = std::clamp((RIDGE_HALF - std::fabs(raw)) / RIDGE_HALF,
                              0.f, 1.f);
    }

    // ============================================================
    // Домашняя околица
    // ============================================================
    //
    // Подтяжка стоит ЗДЕСЬ, между шумом и высотой, и это важно:
    // высота считается из полей, и подтянуть её отдельно значило бы
    // получить равнину с горным биомом на ней.
    //
    // Почему вообще: в начале координат у всех миров был один и тот
    // же максимальный хребет. Причина — в шуме (см. noise.h), и она
    // устранена там; но и после этого начало пути доставалось жребию:
    // замер по тремстам зёрнам дал 34 % океана, 8 % гор, 5 % Чёрного
    // леса и 1.7 % вулкана. Игрок просыпался в воде или в горах не
    // «иногда», а в каждом третьем мире.
    //
    // Перебор точки появления такое не лечит: он умеет отвергнуть
    // плохое место, но не умеет сделать хорошее.
    {
        const f32 d = std::sqrt(fx * fx + fz * fz);
        const f32 w = homeWeight(d);
        if (w > 0.f) {
            const HomeClimate& hc = HOME_CLIMATE[(u32)impl_->home];
            auto toward = [w](f32 value, f32 target) {
                return value + (target - value) * w;
            };
            // Суша, и заведомо выше уровня моря.
            s.continent   = toward(s.continent, 0.30f);
            s.temperature = toward(s.temperature, hc.temperature);
            s.humidity    = toward(s.humidity, hc.humidity);
            // Пологий рельеф: эрозия высока, пиков нет.
            s.erosion     = toward(s.erosion, 0.85f);
            s.peaks       = toward(s.peaks, 0.f);
            // Ни порчи, ни поднятия коры: ни Чёрного леса, ни хребта.
            s.weird       = toward(s.weird, -0.40f);
            s.uplift     *= (1.f - w);
        }
    }

    // Модификатор высоты: континент + горы, океаны глубже суши
    const f32 continentH = s.continent > 0.f ? s.continent * 45.f
                                             : s.continent * 70.f;
    const f32 mountainH  = s.peaks * (1.f - s.erosion) * 55.f;

    // Хребет поднимает сам по себе, а пики делают его рваным: ровное
    // поднятие дало бы плато, а не горы.
    const f32 upliftH = s.uplift * s.uplift * (46.f + s.peaks * 48.f);

    s.heightMod = continentH + mountainH + upliftH;

    s.biome = Plains;   // уточняется в classify()
    return s;
}

void BiomeField::classify(Sample& s, i32 surfaceY) const {
    // Падение температуры с высотой (lapse rate)
    const f32 tempAdjusted = s.temperature - (f32)std::max(0, surfaceY - 40) * 0.008f;

    BiomeId b;
    if (surfaceY < 24 && s.continent < -0.05f)            b = Ocean;
    else if (surfaceY < 28 && s.continent < 0.05f)        b = Beach;
    // Вулкан — это ГОРА в жарком краю, поэтому спрашивается раньше
    // гор: иначе всякая огненная вершина оказывалась бы просто
    // вершиной. Температура берётся сырая, без поправки на высоту:
    // вулкан горяч не от климата, а изнутри, и на трёхстах метрах
    // поправка съедала бы ровно то, по чему его и узнают.
    else if (s.uplift > 0.60f && s.temperature > 0.34f)   b = Volcanic;
    else if (surfaceY > 76 || s.uplift > 0.45f)           b = Mountains;
    // Порча сильнее климата, но слабее моря и гор: Чёрный лес растёт
    // на суше и не карабкается на скалы. Порог высокий — такие места
    // должны попадаться, а не встречаться на каждом шагу.
    else if (s.weird > 0.52f)                             b = Blight;
    else if (s.continent > 0.55f && tempAdjusted > 0.3f)  b = Volcanic;
    else if (s.humidity < -0.35f && tempAdjusted > 0.25f) b = Desert;
    else if (s.humidity < -0.10f && tempAdjusted > 0.15f) b = Savanna;
    else if (tempAdjusted < -0.45f)                       b = Tundra;
    else if (tempAdjusted < -0.10f && s.humidity > 0.0f)  b = Taiga;
    else if (s.humidity > 0.35f && tempAdjusted > 0.0f && s.erosion > 0.5f) b = Swamp;
    else if (s.humidity > 0.05f)                          b = Forest;
    else                                                  b = Plains;

    s.biome = b;
}

BiomeField::Sample BiomeField::sample(i32 x, i32 z, i32 surfaceY) const {
    Sample s = fields(x, z);
    classify(s, surfaceY);
    return s;
}

const BiomeDef& BiomeField::def(BiomeId b) const {
    return BIOMES[b];
}

} // namespace world

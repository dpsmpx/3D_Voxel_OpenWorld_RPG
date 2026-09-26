/**
 * @file biome.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "biome.h"
#include "noise.h"
#include "block.h"
#include "landform.h"
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
    { "Ocean",    SAND,    DIRT,    STONE, WATER },
    // Beach
    { "Beach",    SAND,    SAND,    STONE, WATER },
    // Plains
    { "Plains",   GRASS,   DIRT,    STONE, WATER },
    // Forest
    { "Forest",   GRASS,   DIRT,    STONE, WATER },
    // Taiga
    { "Taiga",    GRASS,   DIRT,    STONE, WATER },
    // Desert
    { "Desert",   SAND,    SAND,    STONE, WATER },
    // Savanna
    { "Savanna",  GRASS,   DIRT,    STONE, WATER },
    // Tundra
    { "Tundra",   SNOW,    DIRT,    STONE, ICE },
    // Mountains
    { "Mountains",STONE,   STONE,   STONE, WATER },
    // Swamp
    { "Swamp",    GRASS,   DIRT,    STONE, WATER },
    // Volcanic
    { "Volcanic", STONE,   STONE,   STONE, LAVA },
    // Blight — Чёрный лес. Поверхность — голая земля, а не трава: в
    // Чёрном лесу ничего не растёт, кроме самого леса (густой
    // сухостой, см. floraProfile).
    { "Blight",   DIRT,    DIRT,    STONE, WATER },
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

static inline f32 smoothstep01(f32 a, f32 b, f32 x) {
    const f32 t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

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
    SimplexNoise warp;
    SimplexNoise hills;
    SimplexNoise plateau;
    SimplexNoise region;
    SimplexNoise orient;

    /// Какая околица досталась миру. Выбирается зерном один раз.
    HomeKind home = HomeKind::Plains;
    /// Зерно — ключ кэша узлов решётки: у одного зерна поля одни.
    u64 seed = 0;

    explicit Impl(u64 seed)
        : continent(seed ^ 0x1111),
          temperature(seed ^ 0x2222),
          humidity(seed ^ 0x3333),
          erosion(seed ^ 0x4444),
          peaks(seed ^ 0x5555),
          weird(seed ^ 0x6666),
          uplift(seed ^ 0x7777),
          warp(seed ^ 0x8888),
          hills(seed ^ 0x9999),
          plateau(seed ^ 0xAAAA),
          region(seed ^ 0xBBBB),
          orient(seed ^ 0xCCCC),
          seed(seed)
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

// ============================================================
// Крупные поля — по решётке
// ============================================================
//
// Поля климата и крупных форм меняются на сотнях блоков, а стоили
// по три десятка выборок шума на КАЖДУЮ колонку: больше половины
// цены генерации чанка. Теперь они считаются точно только в узлах
// решётки с шагом LATTICE и между узлами берутся билинейно. На
// длине волны в сотни блоков разницы глазом не видно, а колонка
// дешевле вдвое.
//
// Точечный запрос и генерация чанка идут одной дорогой — через узлы
// и одну и ту же формулу смешивания, — поэтому ответ у них совпадает
// до бита. Узлы кэшируются в потоке: соседние колонки делят их.

namespace {

struct LatticeEntry {
    u64 seed = 0;
    i32 nx = 0, nz = 0;
    bool valid = false;
    BiomeField::Sample s{};
};

inline i32 floorDiv8(i32 v) { return v >= 0 ? v / 8 : -((-v + 7) / 8); }

} // namespace

BiomeField::Sample BiomeField::latticeNode(i32 nx, i32 nz) const {
    static thread_local std::array<LatticeEntry, 1024> cache;
    const u32 h = ((u32)nx * 73856093u ^ (u32)nz * 19349663u) & 1023u;
    LatticeEntry& e = cache[h];
    if (!e.valid || e.seed != impl_->seed || e.nx != nx || e.nz != nz) {
        e.s = fieldsExact(nx * LATTICE, nz * LATTICE);
        e.seed = impl_->seed; e.nx = nx; e.nz = nz; e.valid = true;
    }
    return e.s;
}

BiomeField::Sample BiomeField::fields(i32 x, i32 z) const {
    static_assert(LATTICE == 8, "floorDiv8 рассчитан на шаг 8");
    const i32 nx = floorDiv8(x), nz = floorDiv8(z);
    const f32 tx = (f32)(x - nx * LATTICE) * (1.f / (f32)LATTICE);
    const f32 tz = (f32)(z - nz * LATTICE) * (1.f / (f32)LATTICE);

    Sample s;
    if (tx == 0.f && tz == 0.f) {
        // Узел решётки — узлы гидросети ровно такие.
        s = latticeNode(nx, nz);
    } else {
        const Sample a = latticeNode(nx, nz),     b = latticeNode(nx + 1, nz);
        const Sample c = latticeNode(nx, nz + 1), d = latticeNode(nx + 1, nz + 1);
        const f32 w00 = (1.f - tx) * (1.f - tz), w10 = tx * (1.f - tz);
        const f32 w01 = (1.f - tx) * tz,         w11 = tx * tz;
        s = a;
        auto mix = [&](f32 Sample::* f) {
            s.*f = a.*f * w00 + b.*f * w10 + c.*f * w01 + d.*f * w11;
        };
        mix(&Sample::continent); mix(&Sample::temperature); mix(&Sample::humidity);
        mix(&Sample::erosion);   mix(&Sample::peaks);       mix(&Sample::weird);
        mix(&Sample::uplift);    mix(&Sample::hills);       mix(&Sample::plateau);
        mix(&Sample::basin);     mix(&Sample::region);      mix(&Sample::orient);
    }
    // Дрожание границ — мелкого масштаба, его по решётке не берут.
    s.edge = impl_->weird.fbm3D((f32)x * 0.03f, 7.3f, (f32)z * 0.03f, 2);
    s.biome = Plains;
    return s;
}

BiomeField::Sample BiomeField::fieldsExact(i32 x, i32 z) const {
    const f32 fx = (f32)x;
    const f32 fz = (f32)z;

    Sample s{};
    // Континенты — очень низкочастотный шум, со сдвигом точки выборки
    // другим шумом: без него берег обводит изолинию гладкого поля и
    // выходит овалом, с ним — заливами, мысами и полуостровами.
    const f32 wx = impl_->warp.fbm3D(fx * 0.0009f, 0.f, fz * 0.0009f, 2) * 190.f;
    const f32 wz = impl_->warp.fbm3D(fx * 0.0009f + 41.3f, 0.f, fz * 0.0009f - 17.9f, 2) * 190.f;
    s.continent = impl_->continent.fbm3D((fx + wx) * 0.0004f, 0.f, (fz + wz) * 0.0004f,
                                         4, 2.f, 0.5f);
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
    // иначе это либо рощица, либо полмира. Выборка — со сдвигом, как у
    // континента: без него верхушка поля, чуть перевалившая порог,
    // обводилась ромбом с прямыми краями.
    s.weird = impl_->weird.fbm3D((fx + wx) * 0.0008f, 0.f, (fz - wz) * 0.0008f, 3);

    // ---- Характер области и направление её хребтов ----
    //
    // Самые медленные поля мира после континента. Направление плавно
    // поворачивается от области к области: в одной горы тянутся с
    // запада на восток, в соседней — с юга на север, и швов между
    // ними нет, потому что угол непрерывен.
    s.region = std::clamp(impl_->region.fbm3D(fx * 0.00028f, 0.f, fz * 0.00028f, 2) * 0.9f
                          + 0.5f, 0.f, 1.f);
    s.orient = impl_->orient.fbm3D(fx * 0.00016f, 0.f, fz * 0.00016f, 2) * 3.14159f;

    // ---- Горная страна ----
    //
    // Раньше хребтом была узкая полоса вокруг НУЛЯ шума: лента в сто
    // блоков шириной, извивающаяся через всю карту, — не горы, а
    // стена-змея. Теперь маска — вытянутые массивы: по оси области
    // шум вдвое медленнее, чем поперёк, и горная страна идёт полосой
    // в несколько сотен блоков шириной и километры длиной. Гребни
    // внутри неё рисует LandformNoise.
    {
        const OrientFrames of = OrientFrames::of(s.orient);
        f32 raw = 0.f;
        for (i32 k = 0; k < 3; ++k) {
            if (of.w[k] <= 0.f) continue;
            const f32 u =  fx * OrientFrames::COS[k] + fz * OrientFrames::SIN[k];
            const f32 v = -fx * OrientFrames::SIN[k] + fz * OrientFrames::COS[k];
            raw += of.w[k] * impl_->uplift.fbm3D(u * 0.00026f, 11.f * (f32)k, v * 0.00056f, 3);
        }
        raw *= of.norm;   // смесь двух полей тише каждого — возвращаем размах
        s.uplift = smoothstep01(0.14f, 0.50f, raw) *
                   smoothstep01(-0.04f, 0.14f, s.continent);
    }

    // ---- Холмы, плато, низины ----
    s.hills = smoothstep01(-0.32f, 0.22f,
                           impl_->hills.fbm3D(fx * 0.0010f, 0.f, fz * 0.0010f, 2));
    {
        // Плато — край резкий, иначе это просто высокий холм. Тяготеет
        // к сухим краям: столовые горы стоят в степях, а не в болотах.
        const f32 pn = impl_->plateau.fbm3D(fx * 0.0007f, 0.f, fz * 0.0007f, 2);
        const f32 dry = 0.35f + 0.65f * smoothstep01(0.30f, -0.15f, s.humidity);
        s.plateau = smoothstep01(0.24f, 0.31f, pn * dry + (1.f - dry) * -0.2f) *
                    (1.f - s.uplift);
    }
    s.basin = smoothstep01(0.12f, 0.48f, s.humidity) *
              smoothstep01(0.55f, 0.85f, s.erosion) *
              (1.f - s.uplift) * (1.f - 0.7f * s.hills);

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
            // Ни плато с обрывами, ни низины с болотом; холмы — мягче.
            s.plateau    *= (1.f - w);
            s.basin      *= (1.f - w);
            s.hills      *= (1.f - 0.6f * w);
        }
    }

    s.biome = Plains;   // уточняется в classify()
    return s;
}

void BiomeField::classify(Sample& s, i32 surfaceY) const {
    // Падение температуры с высотой (lapse rate). Дрожание порогов —
    // только здесь: изрезанная граница, а не другой климат.
    const f32 tempAdjusted = s.temperature - (f32)std::max(0, surfaceY - 40) * 0.008f
                           + s.edge * 0.05f;
    const f32 humid = s.humidity - s.edge * 0.05f;

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
    else if (s.weird + s.edge * 0.06f > 0.52f)            b = Blight;
    else if (s.continent > 0.55f && tempAdjusted > 0.3f)  b = Volcanic;
    else if (humid < -0.35f && tempAdjusted > 0.25f) b = Desert;
    else if (humid < -0.10f && tempAdjusted > 0.15f) b = Savanna;
    else if (tempAdjusted < -0.45f)                       b = Tundra;
    else if (tempAdjusted < -0.10f && humid > 0.0f)  b = Taiga;
    // Болото — в сырой низине: там, где вода стоит, а не просто там,
    // где влажно. Низина (basin) делает болото вероятнее.
    else if (tempAdjusted > 0.0f &&
             ((humid > 0.35f && s.erosion > 0.5f) || (humid > 0.18f && s.basin > 0.55f)))
                                                          b = Swamp;
    else if (humid > 0.05f)                          b = Forest;
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

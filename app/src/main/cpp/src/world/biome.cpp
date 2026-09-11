/**
 * @file biome.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "biome.h"
#include "noise.h"
#include "block.h"
#include <cmath>
#include <array>

namespace world {

// ============================================================
// Таблица биомов. Значения подобраны чтобы давать разнообразный
// но не шизофреничный мир.
// ============================================================
static const std::array<BiomeDef, BIOME_COUNT> BIOMES = {{
    // Ocean
    { "Ocean",    SAND,    DIRT,    STONE, WATER, 0.00f, TreeType::None,   0.0f, 0,  -0.3f },
    // Beach
    { "Beach",    SAND,    SAND,    STONE, WATER, 0.02f, TreeType::Palm,   2.0f, 0,   0.3f },
    // Plains
    { "Plains",   GRASS,   DIRT,    STONE, WATER, 0.15f, TreeType::Oak,    8.0f, 2,   0.2f },
    // Forest
    { "Forest",   GRASS,   DIRT,    STONE, WATER, 1.80f, TreeType::Oak,   12.0f, 3,   0.1f },
    // Taiga
    { "Taiga",    GRASS,   DIRT,    STONE, WATER, 1.20f, TreeType::Pine,   6.0f, 4,  -0.5f },
    // Desert
    { "Desert",   SAND,    SAND,    STONE, WATER, 0.05f, TreeType::Cactus, 0.3f, 0,   0.6f },
    // Savanna
    { "Savanna",  GRASS,   DIRT,    STONE, WATER, 0.25f, TreeType::Dead,   4.0f, 1,   0.5f },
    // Tundra
    { "Tundra",   SNOW,    DIRT,    STONE, ICE,   0.05f, TreeType::Pine,   0.5f, 3,  -0.8f },
    // Mountains
    { "Mountains",STONE,   STONE,   STONE, WATER, 0.10f, TreeType::Pine,   0.5f, 20, -0.4f },
    // Swamp
    { "Swamp",    GRASS,   DIRT,    STONE, WATER, 0.40f, TreeType::Dead,  10.0f, -2,  0.4f },
    // Volcanic
    { "Volcanic", STONE,   STONE,   STONE, LAVA,  0.00f, TreeType::None,   0.0f, 15,  0.8f },
}};

struct BiomeField::Impl {
    SimplexNoise continent;
    SimplexNoise temperature;
    SimplexNoise humidity;
    SimplexNoise erosion;
    SimplexNoise peaks;
    SimplexNoise weird;

    explicit Impl(u64 seed)
        : continent(seed ^ 0x1111),
          temperature(seed ^ 0x2222),
          humidity(seed ^ 0x3333),
          erosion(seed ^ 0x4444),
          peaks(seed ^ 0x5555),
          weird(seed ^ 0x6666) {}
};

BiomeField::BiomeField(u64 seed) : impl_(new Impl(seed)) {}
// (в реальном проекте — unique_ptr, но здесь упрощаем для краткости)

static inline f32 remap(f32 v, f32 inMin, f32 inMax, f32 outMin, f32 outMax) {
    f32 t = (v - inMin) / (inMax - inMin);
    if (t < 0.f) t = 0.f;
    if (t > 1.f) t = 1.f;
    return outMin + t * (outMax - outMin);
}

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

    // Модификатор высоты: континент + горы, океаны глубже суши
    const f32 continentH = s.continent > 0.f ? s.continent * 45.f
                                             : s.continent * 70.f;
    const f32 mountainH  = s.peaks * (1.f - s.erosion) * 55.f;
    s.heightMod = continentH + mountainH;

    s.biome = Plains;   // уточняется в classify()
    return s;
}

void BiomeField::classify(Sample& s, i32 surfaceY) const {
    // Падение температуры с высотой (lapse rate)
    const f32 tempAdjusted = s.temperature - (f32)std::max(0, surfaceY - 40) * 0.008f;

    BiomeId b;
    if (surfaceY < 24 && s.continent < -0.05f)            b = Ocean;
    else if (surfaceY < 28 && s.continent < 0.05f)        b = Beach;
    else if (surfaceY > 82)                               b = Mountains;
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

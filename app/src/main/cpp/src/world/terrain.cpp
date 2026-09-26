/**
 * @file terrain.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "terrain.h"
#include "block.h"
#include "chunk.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace world {

TerrainGenerator::TerrainGenerator(u64 seed)
    : biome_(seed),
      landform_(seed ^ 0xA55A),
      cavesA_(seed ^ 0xC0DE),
      cavesB_(seed ^ 0xBEEF),
      flora_(seed ^ 0xF10A),
      seed_(seed),
      hydro_(std::make_unique<hydro::Hydrology>(*this, seed))
{}

TerrainGenerator::~TerrainGenerator() = default;


TerrainGenerator::Column TerrainGenerator::column(i32 x, i32 z) const {
    Column col;

    // Шумовые поля не зависят от высоты — считаем их один раз.
    col.climate = biome_.fields(x, z);

    // Высота складывается масштабами — регион, формы, мелочь (см.
    // landform.h), а не суммой шумов одного веса.
    MacroFields mf;
    mf.continent = col.climate.continent;
    mf.humidity  = col.climate.humidity;
    mf.erosion   = col.climate.erosion;
    mf.peaks     = col.climate.peaks;
    mf.mountain  = col.climate.uplift;
    mf.hills     = col.climate.hills;
    mf.plateau   = col.climate.plateau;
    mf.basin     = col.climate.basin;
    mf.region    = col.climate.region;
    mf.orient    = col.climate.orient;
    const LandformSample ls = landform_.sample(mf, x, z);
    f32 h = ls.height;
    col.landform = ls.form;
    col.ridge = ls.ridge;

    // Мягкий потолок вместо жёсткого обрезания.
    //
    // Хребты доходят до полутора сотен, а мир высотой сто двадцать
    // восемь: обрезание по линейке делало из вершин столовые горы —
    // ровные площадки ровно по потолку. Здесь верх сжимается и к
    // потолку только стремится, поэтому вершины разной высоты.
    //
    // Сжатие экспонентой, а не дробью: дробь прижимала всё выше
    // сотни к ста десяти, и вершины выходили столами под снегом.
    auto ceilingOf = [](f32 v) {
        if (v > 100.f) v = 100.f + 22.f * (1.f - std::exp(-(v - 100.f) / 22.f));
        return std::clamp(v, 1.f, 124.f);
    };
    h = ceilingOf(h);
    col.heightRoute = ceilingOf(ls.route);
    // Суша для стока — всегда выше моря. Гидросеть решает, где море,
    // по настоящей высоте, а заливает впадины — по этой; узел суши у
    // берега с высотой стока ниже уровня моря получал тот же уровень
    // заливки, что и море, не находил, куда стечь, и река обрывалась
    // в шаге от устья.
    if ((i32)h > SEA_LEVEL)
        col.heightRoute = std::max(col.heightRoute, (f32)SEA_LEVEL + 0.5f);
    col.surface = (i32)h;
    col.heightF = h;

    // Биом уточняется уже по фактической высоте.
    biome_.classify(col.climate, col.surface);

    // Кратер: у вулкана срезана верхушка, и в ней стоит лава.
    // Именно по нему вулкан и отличают от горы — не цветом камня.
    if (col.climate.biome == Volcanic && col.climate.uplift > 0.80f) {
        const f32 k = (col.climate.uplift - 0.80f) / 0.20f;
        const i32 depth = (i32)(k * 15.f);
        if (depth >= 3) {
            const i32 rim = col.surface;
            col.surface -= depth;
            if (col.surface < 2) col.surface = 2;
            col.heightF = (f32)col.surface;
            col.heightRoute = std::min(col.heightRoute, col.heightF);
            // Лава не вровень с краем: полный до краёв кратер
            // читается как лужа, а не как жерло.
            col.lavaTop = rim - 3;
            if (col.lavaTop <= col.surface) col.lavaTop = 0;
        }
    }
    return col;
}

i32 TerrainGenerator::surfaceHeight(i32 x, i32 z) const {
    return column(x, z).surface;
}

BiomeId TerrainGenerator::biomeAt(i32 x, i32 z) const {
    return column(x, z).climate.biome;
}

BiomeField::Sample TerrainGenerator::sampleClimate(i32 x, i32 z, i32 surfaceY) const {
    return biome_.sample(x, z, surfaceY);
}

TerrainGenerator::CaveDensity
TerrainGenerator::caveDensity(f32 x, f32 y, f32 z) const {
    // Две ridged-поверхности: туннель там, где обе близки к единице.
    // По вертикали шум сжат в 1.6 раза — ходы получаются пологими.
    constexpr f32 SCALE = 1.f / 48.f;
    const f32 sx = x * SCALE, sy = y * SCALE * 1.6f, sz = z * SCALE;

    CaveDensity d;
    const f32 a = cavesA_.sample3D(sx, sy, sz);
    const f32 b = cavesB_.sample3D(sx, sy, sz);
    d.tunnel = std::min(1.f - std::fabs(a), 1.f - std::fabs(b));
    d.hall   = cavesA_.fbm3D(x * 0.008f, y * 0.015f, z * 0.008f, 3);
    return d;
}

bool TerrainGenerator::isCave(i32 x, i32 y, i32 z) const {
    return isCaveAt(caveDensity((f32)x, (f32)y, (f32)z), y);
}

bool TerrainGenerator::oreVeinAt(i32 vcx, i32 vcy, i32 vcz, OreVein& out) const {
    constexpr i32 CELL = ORE_CELL;
    if (vcy * CELL > 80) return false;   // нет руды у поверхности гор

    // Детерминированный хэш
    u64 h64 = (u64)(u32)vcx * 0x9E3779B97F4A7C15ULL;
    h64 ^= (u64)(u32)vcy * 0xC4CEB9FE1A85EC53ULL;
    h64 ^= (u64)(u32)vcz * 0xFF51AFD7ED558CCDULL;
    h64 ^= seed_ ^ 0x0AE5ULL;
    h64 ^= h64 >> 33; h64 *= 0xFF51AFD7ED558CCDULL;
    h64 ^= h64 >> 33;
    const u32 h = (u32)h64;

    // 12% ячеек содержат жилу
    if ((h & 0xFF) > 30) return false;
    // Тип руды зависит от высоты
    if (vcy * CELL > 50) out.kind = ((h >> 8) & 3) == 0 ? GOLD_ORE : IRON_ORE;
    else                 out.kind = ((h >> 8) & 3) < 2 ? IRON_ORE : GOLD_ORE;
    // Центр жилы внутри ячейки, радиус 1.5..2.5
    out.center = { (f32)(vcx * CELL) + (f32)(h & 0xF) / 16.f * (f32)CELL,
                   (f32)(vcy * CELL) + (f32)((h >> 8) & 0xF) / 16.f * (f32)CELL,
                   (f32)(vcz * CELL) + (f32)((h >> 16) & 0xF) / 16.f * (f32)CELL };
    out.radius = 1.5f + (f32)((h >> 24) & 0xFF) / 255.f;
    return true;
}

u16 TerrainGenerator::oreAt(i32 x, i32 y, i32 z) const {
    const i32 cx = x >> 2, cy = y >> 2, cz = z >> 2;
    const glm::vec3 p = { (f32)x + 0.5f, (f32)y + 0.5f, (f32)z + 0.5f };
    // 3x3x3 клетки-соседа: жила может заходить в текущую.
    for (i32 ddy = -1; ddy <= 1; ++ddy)
        for (i32 ddz = -1; ddz <= 1; ++ddz)
            for (i32 ddx = -1; ddx <= 1; ++ddx) {
                OreVein v;
                if (!oreVeinAt(cx + ddx, cy + ddy, cz + ddz, v)) continue;
                const glm::vec3 d = p - v.center;
                if (glm::dot(d, d) < v.radius * v.radius) return v.kind;
            }
    return STONE;
}

} // namespace world

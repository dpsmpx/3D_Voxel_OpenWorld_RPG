/**
 * @file flora.cpp
 * @brief Мир: где что растёт — растения и мелкая природная мелочь.
 */
#include "flora.h"
#include "block.h"
#include <algorithm>
#include <cmath>

namespace world {

namespace {

using feat_util::hashXZ;

inline f32 hash01(i32 x, i32 z, u64 salt) {
    return (f32)(hashXZ(x, z, salt) & 0xFFFFFFu) * (1.f / 16777216.f);
}

inline f32 smooth(f32 a, f32 b, f32 x) {
    const f32 t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

/// Деревья умеренного пояса считаются по влажности, а не по биому:
/// лес редеет к полю плавно, а не обрывается по границе биома.
enum : u8 { COVER_TABLE = 0, COVER_TEMPERATE = 1 };

struct ProfileRow {
    FloraProfile p;
    u8 coverMode;
};

//                                   cover  tree               tree2              mix2  patch  under  shrub             grass flowers rocks dead  reeds treeScale
const ProfileRow ROWS[BIOME_COUNT] = {
    /* Ocean    */ { { 0.00f, FloraKind::Palm,     FloraKind::Palm,     0.f,  0.5f, 0.00f, FloraKind::Bush,    0.00f, 0.00f, 0.00f, 0.00f, 0.0f, 1.0f }, COVER_TABLE },
    /* Beach    */ { { 0.07f, FloraKind::Palm,     FloraKind::Palm,     0.f,  0.8f, 0.04f, FloraKind::DryBush, 0.12f, 0.00f, 0.05f, 0.00f, 0.3f, 1.0f }, COVER_TABLE },
    /* Plains   */ { { 0.00f, FloraKind::Oak,      FloraKind::Birch,  0.3f,  0.9f, 0.10f, FloraKind::Bush,    0.55f, 0.35f, 0.04f, 0.02f, 0.5f, 1.0f }, COVER_TEMPERATE },
    /* Forest   */ { { 0.00f, FloraKind::Oak,      FloraKind::Birch,  0.35f, 0.75f, 0.24f, FloraKind::Bush,    0.28f, 0.10f, 0.05f, 0.06f, 0.5f, 1.0f }, COVER_TEMPERATE },
    /* Taiga    */ { { 0.00f, FloraKind::Pine,     FloraKind::Birch,  0.12f, 0.7f, 0.20f, FloraKind::Fern,    0.18f, 0.03f, 0.08f, 0.07f, 0.4f, 1.0f }, COVER_TEMPERATE },
    /* Desert   */ { { 0.035f,FloraKind::Cactus,   FloraKind::Cactus,   0.f,  0.7f, 0.06f, FloraKind::DryBush, 0.02f, 0.00f, 0.06f, 0.00f, 0.3f, 1.0f }, COVER_TABLE },
    /* Savanna  */ { { 0.06f, FloraKind::Acacia,   FloraKind::DeadTree, 0.15f, 0.9f, 0.08f, FloraKind::DryBush, 0.50f, 0.05f, 0.04f, 0.00f, 0.4f, 1.0f }, COVER_TABLE },
    /* Tundra   */ { { 0.05f, FloraKind::Pine,     FloraKind::Pine,     0.f,  0.8f, 0.03f, FloraKind::Fern,    0.12f, 0.02f, 0.12f, 0.00f, 0.3f, 0.6f }, COVER_TABLE },
    /* Mountains*/ { { 0.18f, FloraKind::Pine,     FloraKind::Birch,  0.08f, 0.7f, 0.06f, FloraKind::Fern,    0.30f, 0.12f, 0.20f, 0.02f, 0.3f, 0.9f }, COVER_TABLE },
    /* Swamp    */ { { 0.30f, FloraKind::Oak,      FloraKind::DeadTree, 0.4f, 0.7f, 0.20f, FloraKind::Fern,    0.40f, 0.04f, 0.02f, 0.10f, 1.0f, 0.85f }, COVER_TABLE },
    /* Volcanic */ { { 0.01f, FloraKind::DeadTree, FloraKind::DeadTree, 0.f,  0.5f, 0.02f, FloraKind::DryBush, 0.02f, 0.00f, 0.25f, 0.00f, 0.0f, 1.0f }, COVER_TABLE },
    // Чёрный лес: густой сухостой без травы и цветов, валежник.
    /* Blight   */ { { 0.62f, FloraKind::DeadTree, FloraKind::DeadTree, 0.f,  0.2f, 0.08f, FloraKind::DryBush, 0.00f, 0.00f, 0.06f, 0.14f, 0.2f, 1.0f }, COVER_TABLE },
};

/// Какая часть ствола в блоках держит столкновение: до кроны.
/// Высота модели — в render/flora_models.cpp; здесь — только то,
/// во что упирается идущий.
f32 trunkBlocksOf(FloraKind k) {
    switch (k) {
        case FloraKind::Oak:      return 2.2f;
        case FloraKind::Birch:    return 3.0f;
        case FloraKind::Pine:     return 2.0f;
        case FloraKind::Acacia:   return 2.4f;
        case FloraKind::Palm:     return 4.0f;
        case FloraKind::DeadTree: return 3.0f;
        case FloraKind::Cactus:   return 1.8f;
        default:                  return 0.f;
    }
}

u8 packScale(f32 s) { return (u8)std::clamp((s - 0.5f) * (255.f / 0.8f), 0.f, 255.f); }

// ---- Вероятности мелочи: одни и те же для генерации и для карты ----

/// Подлесок гуще на опушке (доля деревьев около половины) и у воды.
f32 understoryChance(const FloraProfile& p, f32 cover, f32 near) {
    const f32 edge = std::min(1.f, cover * (1.f - cover) * 4.f);
    return p.understory * (0.35f + 1.3f * edge + 0.5f * near);
}
/// Под кроной трава реже: тень.
f32 shadeOf(f32 cover) { return 1.f - 0.7f * std::min(1.f, cover * 1.8f); }
f32 grassChance(const FloraProfile& p, f32 cover, f32 humidity) {
    return p.grass * shadeOf(cover) * (0.6f + 0.4f * smooth(-0.4f, 0.3f, humidity));
}
f32 flowerChance(const FloraProfile& p, f32 cover, f32 meadow) {
    return p.flowers * meadow * shadeOf(cover) * 0.9f;
}
/// Камни — на крутизне, на щебне и по гребням.
f32 rockChance(const FloraProfile& p, i32 slope, bool stony, f32 ridge) {
    return p.rocks * (0.5f + 0.35f * (f32)std::min(slope, 3)) + (stony ? 0.25f : 0.f) + 0.3f * ridge;
}
/// Цветочные поляны: пятна в полсотни блоков.
f32 meadowAt(const SimplexNoise& n, i32 wx, i32 wz) {
    return smooth(0.30f, 0.65f, n.sample3D((f32)wx * 0.02f, 31.f, (f32)wz * 0.02f) * 0.5f + 0.5f);
}

struct Placer {
    Chunk& c;
    const FeatureContext& ctx;
    const TerrainGenerator& terrain;
    i32 bx, bz;
    u64 seed;

    i32 slopeAt(i32 lx, i32 lz) const {
        if (!ctx.padded) return 0;
        constexpr i32 PW = CHUNK_SIZE + 2;
        auto P = [&](i32 x, i32 z) { return (i32)ctx.padded[(usize)(x + 1) * PW + (usize)(z + 1)]; };
        const i32 h = P(lx, lz);
        return std::max(std::max(std::abs(P(lx + 1, lz) - h), std::abs(P(lx - 1, lz) - h)),
                        std::max(std::abs(P(lx, lz + 1) - h), std::abs(P(lx, lz - 1) - h)));
    }
    i32 groundY(i32 lx, i32 lz) const {
        const usize k = (usize)lx * CHUNK_SIZE + lz;
        return ctx.ground ? ctx.ground[k] : ctx.columnAt(lx, lz, bx + lx, bz + lz).surface;
    }
    bool wet(i32 lx, i32 lz) const {
        return ctx.wet && ctx.wet[(usize)lx * CHUNK_SIZE + lz];
    }
    f32 near(i32 lx, i32 lz) const {
        return ctx.near ? (f32)ctx.near[(usize)lx * CHUNK_SIZE + lz] * (1.f / 255.f) : 0.f;
    }
    u16 at(i32 lx, i32 y, i32 lz) const {
        if ((u32)y >= (u32)CHUNK_SIZE_Y) return AIR;
        return c.voxels[chunkIndex(lx, y, lz)];
    }

    /// Кандидатуры по мировой сетке клеток размера cell: одна на
    /// клетку, сдвиг внутри клетки по хэшу с отступом margin от краёв.
    template <class Fn>
    void cells(i32 cell, i32 margin, u64 salt, Fn&& fn) const {
        const i32 cx0 = (i32)std::floor((f32)bx / (f32)cell);
        const i32 cz0 = (i32)std::floor((f32)bz / (f32)cell);
        const i32 cx1 = (i32)std::floor((f32)(bx + CHUNK_SIZE - 1) / (f32)cell);
        const i32 cz1 = (i32)std::floor((f32)(bz + CHUNK_SIZE - 1) / (f32)cell);
        const i32 span = std::max(1, cell - 2 * margin);
        for (i32 gz = cz0; gz <= cz1; ++gz)
            for (i32 gx = cx0; gx <= cx1; ++gx) {
                const u32 h = hashXZ(gx, gz, seed ^ salt);
                const i32 wx = gx * cell + margin + (i32)((h & 0xFFu) % (u32)span);
                const i32 wz = gz * cell + margin + (i32)(((h >> 8) & 0xFFu) % (u32)span);
                const i32 lx = wx - bx, lz = wz - bz;
                if ((u32)lx >= (u32)CHUNK_SIZE || (u32)lz >= (u32)CHUNK_SIZE) continue;
                fn(lx, lz, wx, wz, h);
            }
    }

    void add(FloraKind k, i32 lx, i32 lz, i32 y, u32 h, f32 scale, u8 variant, u8 trunk = 0,
             bool centred = false) {
        FloraInstance f;
        f.lx = (u8)lx; f.lz = (u8)lz; f.y = (i16)y;
        f.kind = k;
        f.variant = variant;
        f.yaw = (u8)(h >> 16);
        f.scale = packScale(scale);
        f.offset = centred ? 0x88 : (u8)(h >> 24);
        f.trunk = trunk;
        c.flora.push_back(f);
    }
};

} // namespace

const FloraProfile& floraProfile(BiomeId b) { return ROWS[b].p; }

bool floraGroundOk(FloraKind k, u16 g) {
    switch (k) {
        case FloraKind::Cactus:
        case FloraKind::DryBush:
            return g == SAND || g == DRY_GRASS || g == DIRT || g == GRAVEL;
        case FloraKind::Palm:
            return g == SAND || g == GRASS || g == DRY_GRASS;
        case FloraKind::Grass:
        case FloraKind::TallGrass:
        case FloraKind::Flowers:
            return g == GRASS || g == DRY_GRASS;
        case FloraKind::Reeds:
            return g == GRASS || g == DIRT || g == SAND || g == GRAVEL || g == DRY_GRASS;
        case FloraKind::Pebbles:
        case FloraKind::Rock:
            return g == STONE || g == GRAVEL || g == GRASS || g == SAND || g == DIRT ||
                   g == DRY_GRASS || g == SNOW;
        case FloraKind::Mushroom:
        case FloraKind::Fern:
            return g == GRASS || g == DIRT;
        default:   // деревья, кусты, коряги, пни
            return g == GRASS || g == DIRT || g == DRY_GRASS || g == SNOW;
    }
}

f32 floraGrove(const TerrainGenerator& terrain, i32 wx, i32 wz) {
    const SimplexNoise& n = terrain.floraNoise();
    const f32 fx = (f32)wx, fz = (f32)wz;
    // Сдвиг точки выборки: края рощ изрезаны, а не обведены циркулем.
    const f32 w = n.sample3D(fx * 0.004f + 3.3f, 11.f, fz * 0.004f - 1.7f) * 35.f;
    const f32 v = n.fbm3D((fx + w) * 0.0065f, 5.f, (fz - w) * 0.0065f, 2);
    return std::clamp(v * 0.95f + 0.5f, 0.f, 1.f);
}

f32 floraTreeCover(const TerrainGenerator& terrain, const TerrainGenerator::Column& col,
                   i32 wx, i32 wz, f32 near)
{
    const ProfileRow& row = ROWS[col.climate.biome];
    const FloraProfile& p = row.p;
    f32 base;
    if (row.coverMode == COVER_TEMPERATE) {
        // Лес по влажности: у сухого края поля — отдельные рощицы, у
        // сырого — сплошной массив. Через границу биома плавно.
        base = 0.06f + 0.62f * smooth(-0.18f, 0.40f, col.climate.humidity);
    } else {
        base = p.cover;
    }
    if (base <= 0.f) return 0.f;
    // Рощи и поляны: у одних биомов лес ровный, у других — пятнами.
    const f32 g = floraGrove(terrain, wx, wz);
    const f32 grove = smooth(0.22f, 0.78f, g) * 1.7f;
    f32 c = base * (1.f + (grove - 1.f) * p.patchiness);
    // Вода: у реки гуще, в сухом краю — вдвое (пойменный лес).
    const bool dry = col.climate.humidity < -0.1f;
    c *= 1.f + near * (dry ? 2.0f : 0.6f);
    // Граница леса в горах.
    c *= 1.f - smooth(80.f, 92.f, (f32)col.surface);
    return std::clamp(c, 0.f, 0.85f);
}

f32 floraDecorDensity(const TerrainGenerator& terrain, const TerrainGenerator::Column& col,
                       i32 wx, i32 wz, f32 near)
{
    const FloraProfile& p = floraProfile(col.climate.biome);
    const f32 cv = floraTreeCover(terrain, col, wx, wz, near);
    // Клетки разного размера: трава и цветы — 2x2, подлесок — 3x3,
    // камни — 4x4. Приводим к «штук на блок».
    const f32 perBlock = (grassChance(p, cv, col.climate.humidity) +
                          flowerChance(p, cv, meadowAt(terrain.floraNoise(), wx, wz))) / 4.f +
                         understoryChance(p, cv, near) / 9.f +
                         rockChance(p, 1, false, col.ridge) / 16.f;
    return std::clamp(perBlock * 4.f, 0.f, 1.f);
}

void placeFlora(Chunk& chunk, const FeatureContext& ctx) {
    chunk.flora.clear();
    if (!ctx.terrain) return;
    const TerrainGenerator& terrain = *ctx.terrain;
    Placer P{ chunk, ctx, terrain, chunk.coord.x * CHUNK_SIZE, chunk.coord.z * CHUNK_SIZE, ctx.seed };
    const SimplexNoise& noise = terrain.floraNoise();

    // Доля деревьев по колонкам: её спрашивают все классы — подлесок
    // гуще на опушке, трава реже под кроной.
    static thread_local f32 cover[CHUNK_SIZE * CHUNK_SIZE];
    for (i32 lx = 0; lx < CHUNK_SIZE; ++lx)
        for (i32 lz = 0; lz < CHUNK_SIZE; ++lz) {
            const auto col = ctx.columnAt(lx, lz, P.bx + lx, P.bz + lz);
            cover[lx * CHUNK_SIZE + lz] =
                floraTreeCover(terrain, col, P.bx + lx, P.bz + lz, P.near(lx, lz));
        }
    auto coverAt = [&](i32 lx, i32 lz) { return cover[lx * CHUNK_SIZE + lz]; };

    // ---- деревья: клетки 4x4, стволы не ближе двух блоков ----
    P.cells(4, 1, 0x7EE51ull, [&](i32 lx, i32 lz, i32 wx, i32 wz, u32 h) {
        const f32 cv = coverAt(lx, lz);
        if (cv <= 0.f || hash01(wx, wz, P.seed ^ 0xA11CEull) >= cv) return;
        if (P.wet(lx, lz) || P.slopeAt(lx, lz) >= 3) return;
        const auto col = ctx.columnAt(lx, lz, wx, wz);
        const FloraProfile& pr = floraProfile(col.climate.biome);
        FloraKind kind = pr.tree;
        if (ROWS[col.climate.biome].coverMode == COVER_TEMPERATE) {
            // Состав леса — по теплу: к холоду ели, в прохладе берёзы.
            const f32 t = col.climate.temperature - (f32)std::max(0, col.surface - 40) * 0.008f;
            const f32 pine = smooth(0.10f, -0.22f, t);
            const f32 birch = 0.3f * smooth(-0.30f, 0.05f, t) * (1.f - pine);
            const f32 r = hash01(wx, wz, P.seed ^ 0xB12CAull);
            kind = r < pine ? FloraKind::Pine : (r < pine + birch ? FloraKind::Birch : FloraKind::Oak);
        } else if (pr.mix2 > 0.f && hash01(wx, wz, P.seed ^ 0xB12CAull) < pr.mix2) {
            kind = pr.tree2;
        }
        // В горах внизу — лиственные, выше — ели.
        if (col.climate.biome == Mountains && col.surface < 58 &&
            hash01(wx, wz, P.seed ^ 0x3A11ull) < 0.5f) kind = FloraKind::Oak;

        const i32 y = P.groundY(lx, lz);
        if (!floraGroundOk(kind, P.at(lx, y - 1, lz))) return;
        // В густой роще деревья тянутся к свету — выше; одинокое в
        // поле — ниже и приземистее.
        const f32 scale = pr.treeScale * (0.72f + 0.3f * cv) *
                          (0.85f + 0.35f * hash01(wx, wz, P.seed ^ 0x5CA1Eull));
        const i32 trunk = std::max(1, (i32)std::lround(trunkBlocksOf(kind) * scale));
        // Ствол и место под крону — свободны.
        for (i32 k = 0; k < trunk + 2; ++k)
            if (P.at(lx, y + k, lz) != AIR) return;
        if (y + trunk + 2 >= CHUNK_SIZE_Y) return;
        if (buildingNear(wx, wz, P.seed, &terrain)) return;

        const u16 core = kind == FloraKind::Cactus ? CACTUS_CORE : TRUNK;
        for (i32 k = 0; k < trunk; ++k) chunk.voxels[chunkIndex(lx, y + k, lz)] = core;
        const u8 variant = (u8)((h >> 20) % FLORA_VARIANTS[(u32)kind]);
        P.add(kind, lx, lz, y, h, scale, variant, (u8)trunk, true);
    });

    // ---- подлесок: клетки 3x3; гуще на опушке ----
    P.cells(3, 0, 0xB05B05ull, [&](i32 lx, i32 lz, i32 wx, i32 wz, u32 h) {
        const auto col = ctx.columnAt(lx, lz, wx, wz);
        const FloraProfile& pr = floraProfile(col.climate.biome);
        if (pr.understory <= 0.f) return;
        if (hash01(wx, wz, P.seed ^ 0xB0511ull) >= understoryChance(pr, coverAt(lx, lz), P.near(lx, lz)))
            return;
        if (P.wet(lx, lz) || P.slopeAt(lx, lz) >= 3) return;
        const i32 y = P.groundY(lx, lz);
        const FloraKind kind = pr.shrub;
        if (!floraGroundOk(kind, P.at(lx, y - 1, lz)) || P.at(lx, y, lz) != AIR) return;
        if (kind == FloraKind::Bush && P.at(lx, y + 1, lz) != AIR) return;
        const f32 scale = 0.75f + 0.5f * hash01(wx, wz, P.seed ^ 0x5CA2ull);
        P.add(kind, lx, lz, y, h, scale, (u8)((h >> 20) % FLORA_VARIANTS[(u32)kind]));
    });

    // ---- валежник: коряги, пни, грибы — в лесу ----
    P.cells(6, 1, 0xDEADF1ull, [&](i32 lx, i32 lz, i32 wx, i32 wz, u32 h) {
        const auto col = ctx.columnAt(lx, lz, wx, wz);
        const FloraProfile& pr = floraProfile(col.climate.biome);
        const f32 cv = coverAt(lx, lz);
        if (pr.deadfall <= 0.f || cv < 0.2f) return;
        if (hash01(wx, wz, P.seed ^ 0xDEAD2ull) >= pr.deadfall * cv * 2.5f) return;
        if (P.wet(lx, lz) || P.slopeAt(lx, lz) >= 2) return;
        const i32 y = P.groundY(lx, lz);
        const f32 r = hash01(wx, wz, P.seed ^ 0xDEAD3ull);
        const FloraKind kind = r < 0.4f ? FloraKind::Log : (r < 0.65f ? FloraKind::Stump : FloraKind::Mushroom);
        if (!floraGroundOk(kind, P.at(lx, y - 1, lz)) || P.at(lx, y, lz) != AIR) return;
        if (kind == FloraKind::Log && buildingNear(wx, wz, P.seed, &terrain)) return;
        P.add(kind, lx, lz, y, h, 0.8f + 0.4f * hash01(wx, wz, P.seed ^ 0x5CA3ull),
              (u8)((h >> 20) % FLORA_VARIANTS[(u32)kind]));
    });

    // ---- камни: клетки 4x4; больше на крутизне, гребнях и щебне ----
    P.cells(4, 0, 0x50C45ull, [&](i32 lx, i32 lz, i32 wx, i32 wz, u32 h) {
        const auto col = ctx.columnAt(lx, lz, wx, wz);
        const FloraProfile& pr = floraProfile(col.climate.biome);
        const i32 y = P.groundY(lx, lz);
        const u16 g = P.at(lx, y - 1, lz);
        const f32 prob = rockChance(pr, P.slopeAt(lx, lz), g == GRAVEL || g == STONE, col.ridge);
        if (hash01(wx, wz, P.seed ^ 0x50C46ull) >= prob) return;
        if (P.at(lx, y, lz) != AIR) return;
        const FloraKind kind = hash01(wx, wz, P.seed ^ 0x50C47ull) < 0.6f ? FloraKind::Rock
                                                                          : FloraKind::Pebbles;
        if (!floraGroundOk(kind, g)) return;
        P.add(kind, lx, lz, y, h, 0.6f + 0.6f * hash01(wx, wz, P.seed ^ 0x5CA4ull),
              (u8)((h >> 20) % FLORA_VARIANTS[(u32)kind]));
    });

    // ---- тростник: у самой воды, густыми полосами ----
    for (i32 lx = 0; lx < CHUNK_SIZE; ++lx)
        for (i32 lz = 0; lz < CHUNK_SIZE; ++lz) {
            const f32 nr = P.near(lx, lz);
            if (nr < 0.55f) continue;
            const i32 wx = P.bx + lx, wz = P.bz + lz;
            const auto col = ctx.columnAt(lx, lz, wx, wz);
            const FloraProfile& pr = floraProfile(col.climate.biome);
            if (pr.reeds <= 0.f) continue;
            const i32 y = P.groundY(lx, lz);
            if (P.at(lx, y, lz) != AIR || !floraGroundOk(FloraKind::Reeds, P.at(lx, y - 1, lz))) continue;
            // У кромки: рядом вода. Соседа в своём чанке видно по
            // вокселям и по карте рек, соседа за границей — тем же
            // запросом к гидрологии, что сделает его чанк: тростник
            // вдоль берега не обрывается по шву.
            bool edge = false;
            static const i32 DX[4] = { 1, -1, 0, 0 }, DZ[4] = { 0, 0, 1, -1 };
            for (i32 d = 0; d < 4 && !edge; ++d) {
                const i32 nx = lx + DX[d], nz = lz + DZ[d];
                if ((u32)nx < (u32)CHUNK_SIZE && (u32)nz < (u32)CHUNK_SIZE) {
                    edge = P.wet(nx, nz) || P.at(nx, y - 1, nz) == WATER || P.at(nx, y, nz) == WATER;
                } else {
                    bool w = false;
                    ctx.groundAt(-1, -1, wx + DX[d], wz + DZ[d], &w);
                    edge = w;
                }
            }
            if (!edge) continue;
            // Полосами: медленный шум вдоль берега решает, где тростник.
            const f32 band = noise.sample3D((f32)wx * 0.05f, 21.f, (f32)wz * 0.05f) * 0.5f + 0.5f;
            if (hash01(wx, wz, P.seed ^ 0x2EED5ull) >= pr.reeds * smooth(0.3f, 0.7f, band)) continue;
            const u32 h = hashXZ(wx, wz, P.seed ^ 0x2EED6ull);
            P.add(FloraKind::Reeds, lx, lz, y, h, 0.8f + 0.4f * hash01(wx, wz, P.seed ^ 0x5CA5ull),
                  (u8)((h >> 20) % FLORA_VARIANTS[(u32)FloraKind::Reeds]));
        }

    // ---- трава и цветы: клетки 2x2; поляны пятнами ----
    P.cells(2, 0, 0x6A55ull, [&](i32 lx, i32 lz, i32 wx, i32 wz, u32 h) {
        const auto col = ctx.columnAt(lx, lz, wx, wz);
        const FloraProfile& pr = floraProfile(col.climate.biome);
        if (pr.grass <= 0.f && pr.flowers <= 0.f) return;
        const i32 y = P.groundY(lx, lz);
        const u16 g = P.at(lx, y - 1, lz);
        if (g != GRASS && g != DRY_GRASS) return;
        if (P.at(lx, y, lz) != AIR) return;
        const f32 cv = coverAt(lx, lz);

        // Цветочные поляны: пятна одного цвета, на прогалинах.
        const f32 fx = (f32)wx, fz = (f32)wz;
        const f32 r = hash01(wx, wz, P.seed ^ 0xF10E7ull);
        if (r < flowerChance(pr, cv, meadowAt(noise, wx, wz))) {
            u8 color = (u8)std::clamp((noise.sample3D(fx * 0.012f, 41.f, fz * 0.012f) * 0.5f + 0.5f) * 4.f,
                                      0.f, 3.f);
            if (((h >> 12) & 7u) == 0u) color = (u8)((h >> 16) & 3u);   // вкрапления чужого цвета
            P.add(FloraKind::Flowers, lx, lz, y, h, 0.8f + 0.4f * hash01(wx, wz, P.seed ^ 0x5CA6ull), color);
            return;
        }
        if (hash01(wx, wz, P.seed ^ 0x6A56ull) >= grassChance(pr, cv, col.climate.humidity)) return;
        const bool tall = hash01(wx, wz, P.seed ^ 0x6A57ull) <
                          (col.climate.biome == Savanna ? 0.65f : 0.25f + 0.3f * P.near(lx, lz));
        const FloraKind kind = tall ? FloraKind::TallGrass : FloraKind::Grass;
        // Цвет пучка — по земле: на выгоревшей траве соломенный, на
        // живой — зелёный (последний вариант модели — сухой).
        const u8 dryVariant = (u8)(FLORA_VARIANTS[(u32)kind] - 1);
        const u8 variant = g == DRY_GRASS ? dryVariant : (u8)((h >> 20) % dryVariant);
        P.add(kind, lx, lz, y, h, 0.75f + 0.5f * hash01(wx, wz, P.seed ^ 0x5CA7ull), variant);
    });

    // Порядок по классу: рендер отбрасывает мелочь дальних чанков
    // целиком, не перебирая её по одной.
    std::stable_sort(chunk.flora.begin(), chunk.flora.end(),
                     [](const FloraInstance& a, const FloraInstance& b) {
                         return (u8)floraClass(a.kind) < (u8)floraClass(b.kind);
                     });
}

void pruneFlora(Chunk& chunk) {
    auto voxel = [&](i32 x, i32 y, i32 z) -> u16 {
        return (u32)y < (u32)CHUNK_SIZE_Y ? chunk.voxels[chunkIndex(x, y, z)] : AIR;
    };
    usize w = 0;
    for (usize i = 0; i < chunk.flora.size(); ++i) {
        const FloraInstance& f = chunk.flora[i];
        bool ok = floraStillStands(f, voxel);
        if (floraIsTree(f.kind)) {
            ok = ok && floraGroundOk(f.kind, voxel(f.lx, f.y - 1, f.lz));
            if (!ok) {
                const u16 core = f.kind == FloraKind::Cactus ? CACTUS_CORE : TRUNK;
                for (i32 k = 0; k < f.trunk; ++k) {
                    const i32 y = f.y + k;
                    if ((u32)y < (u32)CHUNK_SIZE_Y && voxel(f.lx, y, f.lz) == core)
                        chunk.voxels[chunkIndex(f.lx, y, f.lz)] = AIR;
                }
            }
        }
        if (ok) chunk.flora[w++] = f;
    }
    chunk.flora.resize(w);
}

} // namespace world

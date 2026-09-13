/**
 * @file features.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "features.h"
#include "../core/log.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace world {
using namespace feat_util;

// ============================================================
// TREES
//
// Для каждого чанка: biome.treeDensity → целое N. Для i∈[0,N)
// генерируем позицию через hash(chunkX, chunkZ, i).
// Дерево "своё" в этом чанке. Если ствол у края, canopy выйдет
// за границы — эти блоки появятся когда сосед построит себя
// и сам вызовет applyTrees. Чтобы избежать двойных стволов —
// каждая фича работает по СВОЕМУ чанку и не пишет за его пределы.
// Canopy обрезается по границам; сосед добавит её визуально
// корректно, если ствол стоит в нём.
//
// Дублирование решается так: сосед НЕ рисует ствол, потому что
// его treeDensity сгенерирует СВОИ деревья. Визуально это
// выглядит нормально, лишь изредка обрезанные ветви на границе.
// ============================================================
namespace trees {

struct TreeShape {
    u8 trunkHeight;
    u8 trunkBlock;
    u8 leafBlock;
    // Canopy shape — функция от (dy, dx, dz) относительно верхушки ствола.
    // Реализуем три типа: blob (oak), cone (pine), palm (umbrella).
    u8 kind;
};

static TreeShape treeShapeFor(TreeType t, u32 rng) {
    TreeShape s{};
    switch (t) {
        case TreeType::Oak:
            s.trunkHeight = 4 + (rng % 3);  // 4..6
            s.trunkBlock = WOOD;
            s.leafBlock = LEAVES;
            s.kind = 0;
            break;
        case TreeType::Pine:
            s.trunkHeight = 6 + (rng % 4);  // 6..9
            s.trunkBlock = WOOD;
            s.leafBlock = LEAVES;
            s.kind = 1;
            break;
        case TreeType::Palm:
            s.trunkHeight = 5 + (rng % 3);
            s.trunkBlock = WOOD;
            s.leafBlock = LEAVES;
            s.kind = 2;
            break;
        case TreeType::Cactus:
            s.trunkHeight = 2 + (rng % 2);
            s.trunkBlock = WOOD;    // временно; в идеале — CACTUS
            s.leafBlock = AIR;
            s.kind = 3;
            break;
        case TreeType::Dead:
            s.trunkHeight = 3 + (rng % 3);
            s.trunkBlock = WOOD;
            s.leafBlock = AIR;
            s.kind = 4;
            break;
        default: return {};
    }
    return s;
}

static void stampBlob(Chunk& c, i32 bx, i32 by, i32 bz, u8 leaf) {
    // Стандартный oak: 5x3x5 радиусы
    for (i32 dy = -2; dy <= 1; ++dy) {
        i32 r = (dy <= -1) ? 2 : ((dy == 0) ? 2 : 1);
        for (i32 dx = -r; dx <= r; ++dx) {
            for (i32 dz = -r; dz <= r; ++dz) {
                // Скругление
                if (std::abs(dx) == r && std::abs(dz) == r && r > 1) continue;
                if (dy == 1 && (std::abs(dx) == 1 || std::abs(dz) == 1)) continue;
                put(c, bx + dx, by + dy, bz + dz, leaf, /*overwrite=*/false);
            }
        }
    }
}

static void stampCone(Chunk& c, i32 bx, i32 by, i32 bz, u8 leaf, i32 height) {
    // Pine: конус, сужается к вершине
    for (i32 dy = -height + 1; dy <= 0; ++dy) {
        i32 level = dy + height;  // 1..height
        i32 r = std::max(0, (level + 1) / 2);
        if (r > 3) r = 3;
        for (i32 dx = -r; dx <= r; ++dx) {
            for (i32 dz = -r; dz <= r; ++dz) {
                if (std::abs(dx) + std::abs(dz) > r + 1) continue;
                put(c, bx + dx, by + dy, bz + dz, leaf, false);
            }
        }
    }
}

static void stampPalm(Chunk& c, i32 bx, i32 by, i32 bz, u8 leaf) {
    // Umbrella: центральная шапка + 4 листа по сторонам
    for (i32 dx = -2; dx <= 2; ++dx)
        for (i32 dz = -2; dz <= 2; ++dz) {
            if (std::abs(dx) + std::abs(dz) <= 2)
                put(c, bx + dx, by, bz + dz, leaf, false);
        }
    // Свисающие листья по краям
    put(c, bx + 2, by - 1, bz, leaf, false);
    put(c, bx - 2, by - 1, bz, leaf, false);
    put(c, bx, by - 1, bz + 2, leaf, false);
    put(c, bx, by - 1, bz - 2, leaf, false);
}

static void stampTree(Chunk& c, i32 bx, i32 groundY, i32 bz, const TreeShape& s) {
    // Ствол
    for (i32 dy = 0; dy < s.trunkHeight; ++dy)
        put(c, bx, groundY + dy, bz, s.trunkBlock);

    i32 topY = groundY + s.trunkHeight;
    if (s.leafBlock == AIR) return;
    switch (s.kind) {
        case 0: stampBlob(c, bx, topY, bz, s.leafBlock); break;
        case 1: stampCone(c, bx, topY, bz, s.leafBlock, s.trunkHeight); break;
        case 2: stampPalm(c, bx, topY, bz, s.leafBlock); break;
        default: break;
    }
}

} // namespace trees

void applyTrees(Chunk& chunk, const FeatureContext& ctx) {
    const auto& terrain = *ctx.terrain;
    const i32 midX = chunk.coord.x * CHUNK_SIZE + CHUNK_SIZE / 2;
    const i32 midZ = chunk.coord.z * CHUNK_SIZE + CHUNK_SIZE / 2;
    const BiomeDef& biome = terrain.field().def(
        ctx.columnAt(CHUNK_SIZE / 2, CHUNK_SIZE / 2, midX, midZ).climate.biome);

    if (biome.treeType == TreeType::None || biome.treeDensity <= 0.01f) return;

    // N деревьев в чанке — округление с вероятностной добавкой
    f32 d = biome.treeDensity;
    i32 N = (i32)d;
    f32 frac = d - (f32)N;
    u32 h = hashXZ(chunk.coord.x, chunk.coord.z, ctx.seed ^ 0x7EE5);
    if ((f32)(h & 0xFFFF) / 65536.f < frac) ++N;

    for (i32 i = 0; i < N; ++i) {
        u32 rng = hashXZ(chunk.coord.x * 31 + i, chunk.coord.z * 17 + i, ctx.seed ^ 0xA11CE);
        i32 lx = (i32)(rng & 0x1F);          // 0..31
        i32 lz = (i32)((rng >> 5) & 0x1F);

        const i32 wx = chunk.coord.x * CHUNK_SIZE + lx;
        const i32 wz = chunk.coord.z * CHUNK_SIZE + lz;
        const i32 surface = ctx.columnAt(lx, lz, wx, wz).surface;
        if (surface >= CHUNK_SIZE_Y - 12) continue;

        // Проверим, что под деревом подходящий блок (не вода, не песок в океане)
        // (в реальности — читаем из готового chunk.voxels; но там ещё terrain)
        // Здесь проверяем высоту над уровнем моря
        if (surface <= TerrainGenerator::SEA_LEVEL + 1) continue;

        i32 localSurface = surface;
        if (localSurface < 1 || localSurface >= CHUNK_SIZE_Y) continue;

        trees::TreeShape shape = trees::treeShapeFor(biome.treeType, rng);
        if (shape.trunkHeight == 0) continue;

        trees::stampTree(chunk, lx, localSurface, lz, shape);
    }
}

// ============================================================
// CAVES
// ============================================================
void applyCaves(Chunk& chunk, const FeatureContext& ctx) {
    const auto& terrain = *ctx.terrain;
    const i32 baseX = chunk.coord.x * CHUNK_SIZE;
    const i32 baseZ = chunk.coord.z * CHUNK_SIZE;

    // Поле плотности сэмплируется на решётке с шагом STEP и линейно
    // интерполируется. Прямая проверка каждого вокселя стоила пяти
    // выборок шума на воксель — около 200 тысяч на чанк.
    constexpr i32 STEP = 4;
    constexpr i32 GX   = CHUNK_SIZE   / STEP + 1;   // 9
    constexpr i32 GY   = CHUNK_SIZE_Y / STEP + 1;   // 33
    constexpr i32 GZ   = CHUNK_SIZE   / STEP + 1;   // 9

    static thread_local std::vector<TerrainGenerator::CaveDensity> grid;
    grid.resize((usize)GX * GY * GZ);

    for (i32 gx = 0; gx < GX; ++gx)
        for (i32 gy = 0; gy < GY; ++gy)
            for (i32 gz = 0; gz < GZ; ++gz)
                grid[(gx * GY + gy) * GZ + gz] = terrain.caveDensity(
                    (f32)(baseX + gx * STEP),
                    (f32)(gy * STEP),
                    (f32)(baseZ + gz * STEP));

    auto at = [&](i32 gx, i32 gy, i32 gz) -> const TerrainGenerator::CaveDensity& {
        return grid[(gx * GY + gy) * GZ + gz];
    };

    for (i32 x = 0; x < CHUNK_SIZE; ++x) {
        const i32 gx = x / STEP;
        const f32 tx = (f32)(x % STEP) / (f32)STEP;

        for (i32 z = 0; z < CHUNK_SIZE; ++z) {
            const i32 wx = baseX + x, wz = baseZ + z;
            const i32 surface = ctx.columnAt(x, z, wx, wz).surface;
            const i32 gz = z / STEP;
            const f32 tz = (f32)(z % STEP) / (f32)STEP;

            // Карвим только ниже поверхности - 1
            for (i32 y = 2; y < surface - 1 && y < CHUNK_SIZE_Y; ++y) {
                const u16 cur = chunk.voxels[chunkIndex(x, y, z)];
                if (cur == AIR || cur == BEDROCK || cur == WATER) continue;

                const i32 gy = y / STEP;
                const f32 ty = (f32)(y % STEP) / (f32)STEP;

                // Трилинейная интерполяция обоих полей плотности.
                TerrainGenerator::CaveDensity d{0.f, 0.f};
                for (i32 i = 0; i < 8; ++i) {
                    const i32 dx = i & 1, dy = (i >> 1) & 1, dz = (i >> 2) & 1;
                    const f32 w = (dx ? tx : 1.f - tx)
                                * (dy ? ty : 1.f - ty)
                                * (dz ? tz : 1.f - tz);
                    if (w <= 0.f) continue;
                    const auto& g = at(gx + dx, gy + dy, gz + dz);
                    d.tunnel += g.tunnel * w;
                    d.hall   += g.hall   * w;
                }

                if (TerrainGenerator::isCaveAt(d, y))
                    chunk.voxels[chunkIndex(x, y, z)] = AIR;
            }
        }
    }
}

// ============================================================
// ORES
// ============================================================
void applyOres(Chunk& chunk, const FeatureContext& ctx) {
    const auto& terrain = *ctx.terrain;
    const i32 baseX = chunk.coord.x * CHUNK_SIZE;
    const i32 baseZ = chunk.coord.z * CHUNK_SIZE;

    for (i32 x = 0; x < CHUNK_SIZE; ++x) {
        for (i32 z = 0; z < CHUNK_SIZE; ++z) {
            const i32 wx = baseX + x, wz = baseZ + z;
            const i32 surface = ctx.columnAt(x, z, wx, wz).surface;

            for (i32 y = 2; y < surface - 3 && y < CHUNK_SIZE_Y; ++y) {
                u16 cur = chunk.voxels[chunkIndex(x, y, z)];
                if (cur != STONE) continue;

                u16 ore = terrain.oreAt(wx, y, wz);
                if (ore != STONE)
                    chunk.voxels[chunkIndex(x, y, z)] = ore;
            }
        }
    }
}

// ============================================================
// LIQUIDS — вода и лава
// ============================================================
void applyLiquids(Chunk& chunk, const FeatureContext& ctx) {
    const i32 baseX = chunk.coord.x * CHUNK_SIZE;
    const i32 baseZ = chunk.coord.z * CHUNK_SIZE;

    for (i32 x = 0; x < CHUNK_SIZE; ++x) {
        for (i32 z = 0; z < CHUNK_SIZE; ++z) {
            const i32 wx = baseX + x, wz = baseZ + z;
            const i32 surface = ctx.columnAt(x, z, wx, wz).surface;

            // Вода: от текущей поверхности до SEA_LEVEL
            for (i32 y = surface; y <= TerrainGenerator::SEA_LEVEL; ++y) {
                if (y >= CHUNK_SIZE_Y) break;
                u16 cur = chunk.voxels[chunkIndex(x, y, z)];
                if (cur == AIR) chunk.voxels[chunkIndex(x, y, z)] = WATER;
            }

            // Лава: ниже y=8, только в пустотах
            for (i32 y = 2; y <= 8; ++y) {
                u16 cur = chunk.voxels[chunkIndex(x, y, z)];
                if (cur == AIR) chunk.voxels[chunkIndex(x, y, z)] = LAVA;
            }
        }
    }
}

// ============================================================
// STRUCTURES — через super-chunk grid 8×8 чанков
// ============================================================
namespace structs {

constexpr i32 SUPER_CHUNKS = 8;                 // чанков на сторону
constexpr i32 SUPER_BLOCKS = SUPER_CHUNKS * CHUNK_SIZE;  // 256 блоков

// Типы структур
enum Kind : u8 {
    None,
    Village,
    Dungeon,
    Ruin,
    Altar,
};

// Описание структуры: AABB в блоках, тип, seed
struct Layout {
    Kind kind = None;
    glm::ivec3 minBlock{0};
    glm::ivec3 maxBlock{0};
    u32  seed = 0;
};

// Детерминированно выбираем структуру для super-chunk (sx, sz).
Layout layoutFor(i32 sx, i32 sz, u64 worldSeed) {
    Layout L;
    u32 h = hashXZ(sx, sz, worldSeed ^ 0x517);
    // Расположение внутри super-chunk: смещение от 0..100, размер ≤156
    u32 sel = h & 0xFF;

    // 20% — деревня, 15% — руины, 25% — подземелье, 8% — алтарь, иначе пусто
    if (sel < 51)       L.kind = Village;
    else if (sel < 90)  L.kind = Ruin;
    else if (sel < 154) L.kind = Dungeon;
    else if (sel < 174) L.kind = Altar;
    else return L;

    i32 baseX = sx * SUPER_BLOCKS;
    i32 baseZ = sz * SUPER_BLOCKS;
    i32 ox = (i32)((h >> 8) & 0x3F);     // 0..63
    i32 oz = (i32)((h >> 14) & 0x3F);

    // Размер зависит от типа
    i32 halfX = (L.kind == Village) ? 48 : (L.kind == Dungeon) ? 56 : 12;
    i32 halfZ = (L.kind == Village) ? 48 : (L.kind == Dungeon) ? 56 : 12;
    i32 cx = baseX + ox + 32;            // центр
    i32 cz = baseZ + oz + 32;

    L.minBlock = { cx - halfX, 4, cz - halfZ };
    L.maxBlock = { cx + halfX, 80, cz + halfZ };
    L.seed = h;
    return L;
}

// --- Примитивы ---
void box(Chunk& c, i32 wx0, i32 wy0, i32 wz0, i32 wx1, i32 wy1, i32 wz1,
         u16 block, bool overwrite = false)
{
    if (wx0 > wx1) std::swap(wx0, wx1);
    if (wy0 > wy1) std::swap(wy0, wy1);
    if (wz0 > wz1) std::swap(wz0, wz1);
    for (i32 y = wy0; y <= wy1; ++y)
        for (i32 z = wz0; z <= wz1; ++z)
            for (i32 x = wx0; x <= wx1; ++x)
                putWorld(c, x, y, z, block, overwrite);
}

void hollowBox(Chunk& c, i32 wx0, i32 wy0, i32 wz0, i32 wx1, i32 wy1, i32 wz1,
               u16 wall, u16 air = AIR)
{
    for (i32 y = wy0; y <= wy1; ++y)
        for (i32 z = wz0; z <= wz1; ++z)
            for (i32 x = wx0; x <= wx1; ++x) {
                bool edge = (x == wx0 || x == wx1 || z == wz0 || z == wz1 || y == wy0 || y == wy1);
                putWorld(c, x, y, z, edge ? wall : air, edge);
            }
}

void cylinder(Chunk& c, i32 cx, i32 cz, i32 y, i32 radius, u16 block, bool overwrite = true) {
    for (i32 dx = -radius; dx <= radius; ++dx)
        for (i32 dz = -radius; dz <= radius; ++dz) {
            if (dx*dx + dz*dz > radius*radius) continue;
            putWorld(c, cx + dx, y, cz + dz, block, overwrite);
        }
}

// --- Простые структуры ---

// Здесь жили buildWell(), buildHouse() и buildVillage() — остатки
// более раннего подхода к деревням. Их никто не вызывал: настоящая
// расстановка идёт через stampStructure(). buildVillage() состоял из
// одних заглушек и (void)-приведений, а buildHouse() считал позицию
// двери как rng % (w - 2) — при ширине дома в два блока это деление
// на ноль, то есть падение генератора мира на живом устройстве.

} // namespace structs

// ============================================================
// Реальная реализация stampStructure — с доступом к terrain
// ============================================================
namespace {

void stampWell(Chunk& c, i32 wx, i32 wz, i32 wy) {
    for (i32 dx = -2; dx <= 2; ++dx)
        for (i32 dz = -2; dz <= 2; ++dz) {
            bool ring = (std::abs(dx) == 2 || std::abs(dz) == 2);
            for (i32 dy = -1; dy <= 2; ++dy)
                putWorld(c, wx + dx, wy + dy, wz + dz, ring ? STONE : AIR, true);
        }
    putWorld(c, wx, wy, wz, WATER, true);
    for (i32 k = 0; k < 4; ++k) {
        i32 dx = (k & 1) ? 2 : -2;
        i32 dz = (k & 2) ? 2 : -2;
        putWorld(c, wx + dx, wy + 3, wz + dz, WOOD, true);
    }
    for (i32 dx = -2; dx <= 2; ++dx)
        for (i32 dz = -2; dz <= 2; ++dz)
            putWorld(c, wx + dx, wy + 4, wz + dz, WOOD, true);
}

void stampHouse(Chunk& c, const FeatureContext& ctx, i32 wx, i32 wz, i32 w, i32 d, u32 rng) {
    // Найти высоту поверхности в центре дома
    i32 centerX = wx + w/2, centerZ = wz + d/2;
    i32 surf = ctx.terrain->surfaceHeight(centerX, centerZ);

    // Выровнять землю под домом
    for (i32 dx = -1; dx <= w; ++dx)
        for (i32 dz = -1; dz <= d; ++dz) {
            i32 s = ctx.terrain->surfaceHeight(wx + dx, wz + dz);
            for (i32 y = surf; y < s; ++y) putWorld(c, wx+dx, y, wz+dz, AIR, true);
            for (i32 y = s; y < surf; ++y) putWorld(c, wx+dx, y, wz+dz, DIRT, true);
        }

    // Фундамент
    for (i32 dx = 0; dx < w; ++dx)
        for (i32 dz = 0; dz < d; ++dz)
            putWorld(c, wx + dx, surf - 1, wz + dz, STONE, true);

    // Стены + дверь
    i32 doorX = wx + 1 + (rng % (w > 2 ? w - 2 : 1));
    for (i32 y = 0; y < 4; ++y)
        for (i32 dx = 0; dx < w; ++dx)
            for (i32 dz = 0; dz < d; ++dz) {
                bool edge = (dx == 0 || dx == w-1 || dz == 0 || dz == d-1);
                if (!edge) { putWorld(c, wx+dx, surf+y, wz+dz, AIR, true); continue; }
                if (y <= 2 && dz == 0 && wx + dx == doorX) continue;
                putWorld(c, wx + dx, surf + y, wz + dz, WOOD, true);
            }

    // Крыша — слоями наружу
    for (i32 level = 0; level < 2; ++level) {
        i32 x0 = wx - level, x1 = wx + w - 1 + level;
        i32 z0 = wz - level, z1 = wz + d - 1 + level;
        for (i32 dx = x0; dx <= x1; ++dx)
            for (i32 dz = z0; dz <= z1; ++dz)
                putWorld(c, dx, surf + 4 + level, dz, LEAVES, true);
    }
    for (i32 dx = wx - 1; dx <= wx + w; ++dx)
        for (i32 dz = wz - 1; dz <= wz + d; ++dz)
            putWorld(c, dx, surf + 6, dz, LEAVES, true);

    // Факел — светящийся блок в центре
    putWorld(c, wx + w/2, surf + 3, wz + d/2, LAVA, true);
}

void stampVillage(Chunk& c, const FeatureContext& ctx, const structs::Layout& L) {
    i32 cx = (L.minBlock.x + L.maxBlock.x) / 2;
    i32 cz = (L.minBlock.z + L.maxBlock.z) / 2;
    i32 wy = ctx.terrain->surfaceHeight(cx, cz);

    // Колодец в центре
    stampWell(c, cx, cz, wy);

    // 6..10 зданий по кольцу
    u32 h = L.seed;
    i32 count = 6 + (h % 5);
    for (i32 i = 0; i < count; ++i) {
        f32 angle = (f32)i / (f32)count * 6.28318f;
        // Радиус кольца 20..36
        f32 r = 20.f + (f32)((h >> (i % 16)) & 0xF);
        i32 bx = cx + (i32)(std::cos(angle) * r) - 3;
        i32 bz = cz + (i32)(std::sin(angle) * r) - 3;
        // Размер 5..8
        i32 w = 5 + ((h >> i) & 3);
        i32 d = 5 + ((h >> (i + 3)) & 3);
        stampHouse(c, ctx, bx, bz, w, d, h + i * 31);
    }
}

void stampDungeon(Chunk& c, const FeatureContext& ctx, const structs::Layout& L) {
    i32 cx = (L.minBlock.x + L.maxBlock.x) / 2;
    i32 cz = (L.minBlock.z + L.maxBlock.z) / 2;
    // Подземелье: y от 10 до 30
    i32 y0 = 10 + (L.seed % 8);
    i32 y1 = y0 + 8;

    // Коридор: длинный туннель в случайном направлении
    u32 h = L.seed;
    f32 angle = (f32)(h & 0xFFFF) / 65535.f * 6.28318f;
    f32 dx = std::cos(angle), dz = std::sin(angle);
    i32 length = 40 + (h % 20);

    for (i32 step = 0; step < length; ++step) {
        i32 wx = cx + (i32)(dx * step);
        i32 wz = cz + (i32)(dz * step);
        // Прорезать коридор 3x3
        for (i32 ox = -1; ox <= 1; ++ox)
            for (i32 oz = -1; oz <= 1; ++oz)
                for (i32 y = y0; y <= y1; ++y)
                    putWorld(c, wx + ox, y, wz + oz, AIR, true);

        // Каждые 10 шагов — комната
        if (step % 10 == 0) {
            for (i32 ox = -4; ox <= 4; ++ox)
                for (i32 oz = -4; oz <= 4; ++oz)
                    for (i32 y = y0 - 2; y <= y1 + 2; ++y) {
                        bool edge = (std::abs(ox) == 4 || std::abs(oz) == 4 ||
                                     y == y0 - 2 || y == y1 + 2);
                        putWorld(c, wx + ox, y, wz + oz, edge ? STONE : AIR, true);
                    }
            // Сундук-имитация: LAVA как "свет" (в Phase 10 будет реальный)
            putWorld(c, wx, y0 - 1, wz, LAVA, true);
        }
    }
}

void stampRuin(Chunk& c, const FeatureContext& ctx, const structs::Layout& L) {
    i32 cx = (L.minBlock.x + L.maxBlock.x) / 2;
    i32 cz = (L.minBlock.z + L.maxBlock.z) / 2;

    // Обрушенные стены — случайные блоки в радиусе 6
    u32 h = L.seed;
    for (i32 dx = -6; dx <= 6; ++dx)
        for (i32 dz = -6; dz <= 6; ++dz) {
            u32 rng = hashXYZ(cx + dx, 0, cz + dz, h);
            if ((rng & 0x7) > 2) continue;    // ~40% плотность
            i32 height = 1 + (rng >> 4) % 3;
            i32 s = ctx.terrain->surfaceHeight(cx + dx, cz + dz);
            for (i32 y = 0; y < height; ++y)
                putWorld(c, cx + dx, s + y, cz + dz, STONE, true);
        }
}

void stampAltar(Chunk& c, const FeatureContext& ctx, const structs::Layout& L) {
    i32 cx = (L.minBlock.x + L.maxBlock.x) / 2;
    i32 cz = (L.minBlock.z + L.maxBlock.z) / 2;
    i32 wy = ctx.terrain->surfaceHeight(cx, cz);

    // 3x3 каменная платформа
    for (i32 dx = -1; dx <= 1; ++dx)
        for (i32 dz = -1; dz <= 1; ++dz) {
            putWorld(c, cx + dx, wy, cz + dz, STONE, true);
            putWorld(c, cx + dx, wy + 1, cz + dz, AIR, true);
            putWorld(c, cx + dx, wy + 2, cz + dz, AIR, true);
        }
    // Центральная колонна
    putWorld(c, cx, wy + 1, cz, STONE, true);
    putWorld(c, cx, wy + 2, cz, STONE, true);
    // "Кристалл" — LAVA
    putWorld(c, cx, wy + 3, cz, LAVA, true);
}

} // namespace

// ============================================================
// Публичный запрос подземелий: та же детерминированная раскладка,
// что использует applyStructures, но без генерации вокселей.
// ============================================================
DungeonSite dungeonAt(i32 superX, i32 superZ, u64 worldSeed) {
    DungeonSite site;
    const structs::Layout L = structs::layoutFor(superX, superZ, worldSeed);
    if (L.kind != structs::Dungeon) return site;

    site.exists = true;
    site.seed   = L.seed;
    // Зал босса — на дне первой комнаты коридора, там же, где
    // stampDungeon вырезает пол.
    const i32 y0 = 10 + (i32)(L.seed % 8);
    site.center = {
        (L.minBlock.x + L.maxBlock.x) / 2,
        y0,
        (L.minBlock.z + L.maxBlock.z) / 2,
    };
    return site;
}

void applyStructures(Chunk& chunk, const FeatureContext& ctx) {
    // Чанк → диапазон super-chunk'ов
    i32 cx = chunk.coord.x, cz = chunk.coord.z;
    i32 scxMin = (cx / structs::SUPER_CHUNKS) - 2;
    i32 scxMax = (cx / structs::SUPER_CHUNKS) + 2;
    i32 sczMin = (cz / structs::SUPER_CHUNKS) - 2;
    i32 sczMax = (cz / structs::SUPER_CHUNKS) + 2;

    // AABB чанка в блоках
    glm::ivec3 cmin{ cx * CHUNK_SIZE, 0, cz * CHUNK_SIZE };
    glm::ivec3 cmax{ cmin.x + CHUNK_SIZE - 1, CHUNK_SIZE_Y - 1, cmin.z + CHUNK_SIZE - 1 };

    for (i32 sz = sczMin; sz <= sczMax; ++sz) {
        for (i32 sx = scxMin; sx <= scxMax; ++sx) {
            auto L = structs::layoutFor(sx, sz, ctx.seed);
            if (L.kind == structs::None) continue;

            // Пересечение AABB структуры с AABB чанка?
            if (L.maxBlock.x < cmin.x || L.minBlock.x > cmax.x) continue;
            if (L.maxBlock.z < cmin.z || L.minBlock.z > cmax.z) continue;

            switch (L.kind) {
                case structs::Village: stampVillage(chunk, ctx, L); break;
                case structs::Dungeon: stampDungeon(chunk, ctx, L); break;
                case structs::Ruin:    stampRuin(chunk, ctx, L);    break;
                case structs::Altar:   stampAltar(chunk, ctx, L);   break;
                default: break;
            }
        }
    }
}

// ============================================================
// Генерация чанка целиком. Одно место на весь проект: слои рельефа
// и порядок фич больше нигде не выписаны.
// ============================================================
void computeChunkColumns(const TerrainGenerator& terrain, i32 chunkX, i32 chunkZ,
                         std::vector<TerrainGenerator::Column>& out)
{
    out.resize((usize)CHUNK_SIZE * CHUNK_SIZE);
    const i32 baseX = chunkX * CHUNK_SIZE;
    const i32 baseZ = chunkZ * CHUNK_SIZE;
    for (i32 x = 0; x < CHUNK_SIZE; ++x)
        for (i32 z = 0; z < CHUNK_SIZE; ++z)
            out[(usize)x * CHUNK_SIZE + z] = terrain.column(baseX + x, baseZ + z);
}

void generateChunkVoxels(Chunk& chunk, const TerrainGenerator& terrain,
                         const TerrainGenerator::Column* columns, u64 seed)
{
    for (i32 x = 0; x < CHUNK_SIZE; ++x) {
        for (i32 z = 0; z < CHUNK_SIZE; ++z) {
            const auto& col = columns[(usize)x * CHUNK_SIZE + z];
            const i32 surface = col.surface;
            const BiomeDef& biome = terrain.field().def(col.climate.biome);

            for (i32 y = 0; y < CHUNK_SIZE_Y; ++y) {
                u16 id = AIR;
                if (y == 0) {
                    id = BEDROCK;
                } else if (y < surface - 4) {
                    id = biome.stoneBlock;
                } else if (y < surface - 1) {
                    id = biome.subsurfaceBlock;
                } else if (y < surface) {
                    id = biome.surfaceBlock;
                }
                chunk.voxels[chunkIndex(x, y, z)] = id;
            }
        }
    }

    // Порядок важен: пещеры выгрызают толщу, руды садятся в оставшийся
    // камень, жидкости заливают пустоты, структуры и деревья — сверху.
    FeatureContext fctx{ &terrain, seed, columns };
    applyCaves(chunk, fctx);
    applyOres(chunk, fctx);
    applyLiquids(chunk, fctx);
    applyStructures(chunk, fctx);
    applyTrees(chunk, fctx);
}

} // namespace world

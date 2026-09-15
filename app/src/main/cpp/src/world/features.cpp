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

/// Куда смотрит фасад дома.
enum class Facing : u8 { NegZ = 0, PosZ, NegX, PosX };

/// Дом.
///
/// Прежний дом был коробкой из WOOD с плоской нашлёпкой из LEAVES
/// сверху и лужей ЛАВЫ посередине вместо очага. Дверь всегда стояла в
/// стене -Z, независимо от того, где центр деревни, поэтому у половины
/// домов вход смотрел в поле. Окон не было.
///
/// Здесь: фундамент и цоколь из камня, стены из доски, угловые стойки
/// из бревна, окна, соломенная ДВУСКАТНАЯ кровля со свесом, фонарь под
/// коньком и дверь в той стене, что обращена к центру деревни.
void stampHouse(Chunk& c, const FeatureContext& ctx,
                i32 wx, i32 wz, i32 w, i32 d, u32 rng, Facing face)
{
    // Дом меньше четырёх блоков по стороне — это не дом, а будка: в
    // нём не разместить ни двери, ни окна.
    if (w < 4) w = 4;
    if (d < 4) d = 4;

    const i32 centerX = wx + w / 2, centerZ = wz + d / 2;
    const i32 surf = ctx.terrain->surfaceHeight(centerX, centerZ);

    // Выровнять землю под домом и на шаг вокруг.
    for (i32 dx = -1; dx <= w; ++dx)
        for (i32 dz = -1; dz <= d; ++dz) {
            const i32 sh = ctx.terrain->surfaceHeight(wx + dx, wz + dz);
            for (i32 y = surf; y < sh; ++y) putWorld(c, wx+dx, y, wz+dz, AIR, true);
            for (i32 y = sh; y < surf; ++y) putWorld(c, wx+dx, y, wz+dz, DIRT, true);
        }

    // Фундамент под всем домом и пол внутри.
    for (i32 dx = -1; dx <= w; ++dx)
        for (i32 dz = -1; dz <= d; ++dz)
            putWorld(c, wx + dx, surf - 1, wz + dz, STONE, true);

    const i32 wallH = 4;

    // Где дверь: в стене, обращённой к центру деревни. Раньше она
    // всегда была в -Z, и у половины домов вход смотрел в поле.
    const i32 doorX = wx + 1 + (i32)(rng % (u32)(w - 2));
    const i32 doorZ = wz + 1 + (i32)((rng >> 8) % (u32)(d - 2));

    auto isDoor = [&](i32 gx, i32 gz, i32 y) {
        if (y > 1) return false;                 // проём в два блока
        switch (face) {
            case Facing::NegZ: return gz == wz         && gx == doorX;
            case Facing::PosZ: return gz == wz + d - 1 && gx == doorX;
            case Facing::NegX: return gx == wx         && gz == doorZ;
            case Facing::PosX: return gx == wx + w - 1 && gz == doorZ;
        }
        return false;
    };

    // Окна — на середине стены, на уровне глаз, и не в дверной стене
    // рядом с проёмом.
    auto isWindow = [&](i32 gx, i32 gz, i32 y) {
        if (y != 2) return false;
        const bool onX = (gx == wx || gx == wx + w - 1);
        const bool onZ = (gz == wz || gz == wz + d - 1);
        if (!onX && !onZ) return false;
        if (onX && onZ) return false;            // угол — там стойка
        // По одному окну на стену, ближе к середине.
        if (onX) return gz == wz + d / 2;
        return gx == wx + w / 2;
    };

    // Стены.
    for (i32 y = 0; y < wallH; ++y)
        for (i32 dx = 0; dx < w; ++dx)
            for (i32 dz = 0; dz < d; ++dz) {
                const i32 gx = wx + dx, gz = wz + dz;
                const bool edge = (dx == 0 || dx == w-1 || dz == 0 || dz == d-1);
                if (!edge) { putWorld(c, gx, surf + y, gz, AIR, true); continue; }
                if (isDoor(gx, gz, y)) { putWorld(c, gx, surf + y, gz, AIR, true); continue; }
                if (isWindow(gx, gz, y)) { putWorld(c, gx, surf + y, gz, GLASS, true); continue; }

                // Угловые стойки из бревна: они и держат силуэт. Без
                // них стена — однотонная плоскость.
                const bool corner = (dx == 0 || dx == w-1) && (dz == 0 || dz == d-1);
                putWorld(c, gx, surf + y, gz, corner ? WOOD : PLANK, true);
            }

    // Двускатная кровля вдоль длинной стороны, со свесом в один блок.
    //
    // Плоская нашлёпка сверху читалась кустом, а не крышей: именно
    // скат и делает дом домом.
    //
    // Скаты обязаны СОЙТИСЬ. Пока каждый уровень просто сдвигался
    // внутрь на блок, при нечётной ширине они останавливались, не
    // встретившись, и вдоль всего конька оставалась щель: дом стоял с
    // открытым верхом. Поэтому уровень, на котором стороны сошлись,
    // закрывается целиком.
    const bool alongZ = (w <= d);
    const i32 span = alongZ ? w : d;

    for (i32 lvl = 0; ; ++lvl) {
        const i32 y = surf + wallH + lvl;
        const i32 inset = lvl - 1;               // -1 даёт свес
        const i32 lo = inset, hi = span - 1 - inset;
        const bool closing = (lo >= hi - 1);     // стороны сошлись

        if (alongZ) {
            for (i32 gz = wz - 1; gz <= wz + d; ++gz) {
                if (closing) {
                    for (i32 t = lo; t <= hi; ++t)
                        putWorld(c, wx + t, y, gz, THATCH, true);
                } else {
                    putWorld(c, wx + lo, y, gz, THATCH, true);
                    putWorld(c, wx + hi, y, gz, THATCH, true);
                    // Под скатом — воздух, иначе чердак зальётся соломой.
                    for (i32 t = lo + 1; t < hi; ++t)
                        putWorld(c, wx + t, y, gz, AIR, true);
                }
            }
        } else {
            for (i32 gx = wx - 1; gx <= wx + w; ++gx) {
                if (closing) {
                    for (i32 t = lo; t <= hi; ++t)
                        putWorld(c, gx, y, wz + t, THATCH, true);
                } else {
                    putWorld(c, gx, y, wz + lo, THATCH, true);
                    putWorld(c, gx, y, wz + hi, THATCH, true);
                    for (i32 t = lo + 1; t < hi; ++t)
                        putWorld(c, gx, y, wz + t, AIR, true);
                }
            }
        }
        if (closing) break;
    }

    // Фонарь под потолком. Раньше здесь лежала лужа ЛАВЫ.
    putWorld(c, centerX, surf + wallH - 1, centerZ, LANTERN, true);
}


// ============================================================
// Утварь деревни
// ============================================================
//
// Без неё деревня — шесть домов и колодец на пустой траве. Вещи,
// которые ставят вокруг себя живущие люди, и делают место обжитым:
// поленница у стены, стог, грядка, забор, фонарь у дороги.

/// Положить блок на поверхность, не тронув уже построенное.
///
/// Дорожки и утварь кладутся ДО домов, но рельеф под деревней ровняют
/// сами дома. Поэтому пишем только туда, где сейчас земля или трава:
/// иначе дорожка прорежет стену, а стог встанет в комнате.
void putOnGround(Chunk& c, const FeatureContext& ctx, i32 wx, i32 wz, u16 block) {
    // surfaceHeight — первый ВОЗДУШНЫЙ блок над землёй: на нём стоят,
    // а сама земля лежит на блок ниже. Дорожка, положенная на
    // surfaceHeight, висела бы над травой.
    const i32 y = ctx.terrain->surfaceHeight(wx, wz) - 1;
    if (y < 1) return;
    const u16 cur = getWorld(c, wx, y, wz);
    if (cur != GRASS && cur != DIRT && cur != SAND) return;
    putWorld(c, wx, y, wz, block, true);
}

/// Дорожка от точки до точки — прямая, шириной в блок.
void stampPath(Chunk& c, const FeatureContext& ctx,
               i32 x0, i32 z0, i32 x1, i32 z1)
{
    const i32 dx = x1 - x0, dz = z1 - z0;
    const i32 steps = std::max(std::abs(dx), std::abs(dz));
    if (steps <= 0) return;
    for (i32 i = 0; i <= steps; ++i) {
        const i32 x = x0 + dx * i / steps;
        const i32 z = z0 + dz * i / steps;
        putOnGround(c, ctx, x, z, STONE);
    }
}

/// Поленница: дрова вдоль стены.
void stampWoodpile(Chunk& c, const FeatureContext& ctx, i32 wx, i32 wz, bool alongX) {
    const i32 base = ctx.terrain->surfaceHeight(wx, wz);
    for (i32 i = 0; i < 3; ++i) {
        const i32 x = wx + (alongX ? i : 0);
        const i32 z = wz + (alongX ? 0 : i);
        for (i32 y = 0; y < 2; ++y)
            putWorld(c, x, base + y, z, WOOD, true);
    }
}

/// Стог соломы.
void stampHaystack(Chunk& c, const FeatureContext& ctx, i32 wx, i32 wz) {
    const i32 base = ctx.terrain->surfaceHeight(wx, wz);
    for (i32 dx = 0; dx < 2; ++dx)
        for (i32 dz = 0; dz < 2; ++dz)
            for (i32 y = 0; y < 2; ++y)
                putWorld(c, wx + dx, base + y, wz + dz, THATCH, true);
    putWorld(c, wx, base + 2, wz, THATCH, true);
}

/// Грядка: вскопанная земля с всходами.
void stampGarden(Chunk& c, const FeatureContext& ctx, i32 wx, i32 wz, u32 rng) {
    for (i32 dx = 0; dx < 3; ++dx)
        for (i32 dz = 0; dz < 3; ++dz) {
            const i32 x = wx + dx, z = wz + dz;
            const i32 ground = ctx.terrain->surfaceHeight(x, z) - 1;
            if (ground < 1) continue;
            const u16 cur = getWorld(c, x, ground, z);
            if (cur != GRASS && cur != DIRT && cur != SAND) continue;
            putWorld(c, x, ground, z, DIRT, true);
            // Всходы через клетку — сплошная зелень читается кустом.
            if (((u32)(dx + dz * 3) ^ rng) & 1) continue;
            putWorld(c, x, ground + 1, z, LEAVES, true);
        }
}

/// Фонарный столб.
void stampLampPost(Chunk& c, const FeatureContext& ctx, i32 wx, i32 wz) {
    const i32 base = ctx.terrain->surfaceHeight(wx, wz);
    for (i32 y = 0; y < 3; ++y)
        putWorld(c, wx, base + y, wz, WOOD, true);
    putWorld(c, wx, base + 3, wz, LANTERN, true);
}

/// Изгородь: столбы через клетку, а не сплошная стена.
void stampFence(Chunk& c, const FeatureContext& ctx,
                i32 x0, i32 z0, i32 x1, i32 z1)
{
    const i32 dx = x1 - x0, dz = z1 - z0;
    const i32 steps = std::max(std::abs(dx), std::abs(dz));
    if (steps <= 0) return;
    for (i32 i = 0; i <= steps; ++i) {
        const i32 x = x0 + dx * i / steps;
        const i32 z = z0 + dz * i / steps;
        const i32 base = ctx.terrain->surfaceHeight(x, z);
        const u16 ground = getWorld(c, x, base - 1, z);
        if (ground != GRASS && ground != DIRT && ground != SAND) continue;
        // Столб стоит НА земле, а не висит над ней.
        putWorld(c, x, base, z, WOOD, true);
        if (i % 2 == 0) putWorld(c, x, base + 1, z, WOOD, true);
    }
}

void stampVillage(Chunk& c, const FeatureContext& ctx, const structs::Layout& L) {
    const i32 cx = (L.minBlock.x + L.maxBlock.x) / 2;
    const i32 cz = (L.minBlock.z + L.maxBlock.z) / 2;
    const i32 wy = ctx.terrain->surfaceHeight(cx, cz);

    const u32 h = L.seed;
    const i32 count = 6 + (i32)(h % 5);

    // Раскладка домов считается ОДИН раз и переиспользуется: дорожки,
    // утварь и сами дома обязаны знать одни и те же места. Раньше
    // раскладки не было вовсе — дома ставились прямо в цикле, и
    // поставить рядом с домом поленницу было негде.
    struct Plot {
        i32 x, z, w, d;
        Facing face;
        i32 doorX, doorZ;     // блок ПЕРЕД дверью, снаружи
    };
    Plot plots[16];
    const i32 plotCount = (count < 16) ? count : 16;

    for (i32 i = 0; i < plotCount; ++i) {
        const f32 angle = (f32)i / (f32)plotCount * 6.28318f;
        const f32 ca = std::cos(angle), sa = std::sin(angle);
        const f32 r = 20.f + (f32)((h >> (i % 16)) & 0xF);

        Plot& p = plots[i];
        p.w = 5 + (i32)((h >> i) & 3);
        p.d = 5 + (i32)((h >> (i + 3)) & 3);
        p.x = cx + (i32)(ca * r) - p.w / 2;
        p.z = cz + (i32)(sa * r) - p.d / 2;

        // Дом стоит к деревне лицом: дверь в той стене, что ближе к
        // колодцу. Раньше дверь всегда была в стене -Z, и у половины
        // домов вход выходил в поле.
        if (std::fabs(ca) > std::fabs(sa))
            p.face = (ca > 0.f) ? Facing::NegX : Facing::PosX;
        else
            p.face = (sa > 0.f) ? Facing::NegZ : Facing::PosZ;

        const u32 rng = h + (u32)i * 31u;
        const i32 dX = p.x + 1 + (i32)(rng % (u32)(p.w > 2 ? p.w - 2 : 1));
        const i32 dZ = p.z + 1 + (i32)((rng >> 8) % (u32)(p.d > 2 ? p.d - 2 : 1));
        switch (p.face) {
            case Facing::NegZ: p.doorX = dX;           p.doorZ = p.z - 1;       break;
            case Facing::PosZ: p.doorX = dX;           p.doorZ = p.z + p.d;     break;
            case Facing::NegX: p.doorX = p.x - 1;      p.doorZ = dZ;            break;
            default:           p.doorX = p.x + p.w;    p.doorZ = dZ;            break;
        }
    }

    // ---- Дорожки ----
    //
    // Кладутся ПЕРВЫМИ: дома и колодец ставятся поверх и всегда
    // выигрывают. Иначе дорожка прорезала бы порог.
    for (i32 i = 0; i < plotCount; ++i)
        stampPath(c, ctx, plots[i].doorX, plots[i].doorZ, cx, cz);

    // Кольцевая дорожка по деревне: от двери к двери соседа.
    for (i32 i = 0; i < plotCount; ++i) {
        const Plot& a = plots[i];
        const Plot& b = plots[(i + 1) % plotCount];
        stampPath(c, ctx, a.doorX, a.doorZ, b.doorX, b.doorZ);
    }

    // ---- Утварь ----
    //
    // У каждого двора своя: иначе шесть домов обставлены одинаково, и
    // вариация сводится к размеру коробки.
    for (i32 i = 0; i < plotCount; ++i) {
        const Plot& p = plots[i];
        const u32 rng = hashXZ(p.x, p.z, L.seed ^ 0x9D7u);

        // Сторона двора, противоположная двери, — задняя: там и
        // держат дрова, стог и грядку.
        const i32 backX = (p.face == Facing::NegX) ? p.x + p.w + 1
                        : (p.face == Facing::PosX) ? p.x - 3 : p.x;
        const i32 backZ = (p.face == Facing::NegZ) ? p.z + p.d + 1
                        : (p.face == Facing::PosZ) ? p.z - 3 : p.z;

        switch (rng % 4u) {
            case 0: stampWoodpile(c, ctx, backX, backZ, (rng >> 3) & 1u); break;
            case 1: stampHaystack(c, ctx, backX, backZ); break;
            case 2: stampGarden(c, ctx, backX, backZ, rng >> 5); break;
            default:
                // Двор за изгородью.
                stampFence(c, ctx, backX, backZ, backX + 4, backZ);
                stampFence(c, ctx, backX, backZ, backX, backZ + 4);
                break;
        }

        // Фонарь — НА ДОРОЖКЕ, в трёх шагах от двери к колодцу, а не
        // вплотную к стене: под свесом кровли он и стоял бы в тени
        // собственного дома, ради которой его туда и не ставят.
        //
        // Через один двор, чтобы деревня не превратилась в
        // иллюминацию.
        if ((rng >> 7) & 1u) {
            const f32 dx = (f32)(cx - p.doorX), dz = (f32)(cz - p.doorZ);
            const f32 len = std::sqrt(dx * dx + dz * dz);
            if (len > 4.f) {
                const i32 lx = p.doorX + (i32)std::lround(dx / len * 3.f);
                const i32 lz = p.doorZ + (i32)std::lround(dz / len * 3.f);
                stampLampPost(c, ctx, lx, lz);
            }
        }
    }

    // ---- Колодец и дома ----
    stampWell(c, cx, cz, wy);
    for (i32 i = 0; i < plotCount; ++i) {
        const Plot& p = plots[i];
        stampHouse(c, ctx, p.x, p.z, p.w, p.d, h + (u32)i * 31u, p.face);
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

VillageSite villageAt(i32 superX, i32 superZ, u64 worldSeed,
                      const TerrainGenerator* terrain) {
    VillageSite site;
    const structs::Layout L = structs::layoutFor(superX, superZ, worldSeed);
    if (L.kind != structs::Village) return site;

    const i32 cx = (L.minBlock.x + L.maxBlock.x) / 2;
    const i32 cz = (L.minBlock.z + L.maxBlock.z) / 2;

    // Деревня строится на суше.
    //
    // Раскладка структур смотрит только на хэш, поэтому деревни
    // исправно вырастали в океане: дома по колено в воде, дорожки на
    // дне, жители посреди моря. Проверяем не только центр — кольцо
    // домов радиусом до 36 не должно уходить под воду.
    if (terrain) {
        const i32 minLand = TerrainGenerator::SEA_LEVEL + 2;
        if (terrain->surfaceHeight(cx, cz) < minLand) return site;
        const i32 probe[8][2] = {
            { 36, 0 }, { -36, 0 }, { 0, 36 }, { 0, -36 },
            { 26, 26 }, { 26, -26 }, { -26, 26 }, { -26, -26 },
        };
        int wet = 0;
        for (const auto& q : probe)
            if (terrain->surfaceHeight(cx + q[0], cz + q[1]) < minLand) ++wet;
        // Один мокрый край — берег, и это даже хорошо. Половина —
        // деревня в море.
        if (wet > 2) return site;
    }

    site.exists = true;
    site.seed   = L.seed;
    // Колодец ставится ровно в середину раскладки — там же, где его
    // рисует stampVillage.
    site.center = { cx, 0, cz };
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
                case structs::Village:
                    // Тем же правилом, что и villageAt: деревня,
                    // признанная утонувшей, не строится вовсе.
                    if (villageAt(sx, sz, ctx.seed, ctx.terrain).exists)
                        stampVillage(chunk, ctx, L);
                    break;
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

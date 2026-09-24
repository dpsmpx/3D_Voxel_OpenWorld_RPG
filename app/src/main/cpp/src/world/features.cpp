/**
 * @file features.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "features.h"
#include <array>
#include <memory>
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
    u8  trunkHeight;
    u16 trunkBlock;
    u16 leafBlock;
    u8  kind;
    /// Зерно этого дерева: по нему расходятся ветви, наклон и
    /// неровности кроны. Без него все дубы в лесу — один дуб,
    /// размноженный копированием, и лес читается обоями.
    u32 rng;
};

static TreeShape treeShapeFor(TreeType t, u32 rng) {
    TreeShape s{};
    s.rng = rng;
    switch (t) {
        case TreeType::Oak:
            s.trunkHeight = 7 + (rng % 5);   // 7..11
            s.trunkBlock = WOOD;
            s.leafBlock = LEAVES;
            s.kind = 0;
            break;
        case TreeType::Pine:
            s.trunkHeight = 11 + (rng % 6);  // 11..16
            s.trunkBlock = WOOD;
            s.leafBlock = LEAVES;
            s.kind = 1;
            break;
        case TreeType::Palm:
            s.trunkHeight = 8 + (rng % 4);
            s.trunkBlock = WOOD;
            s.leafBlock = LEAVES;
            s.kind = 2;
            break;
        case TreeType::Cactus:
            // Кактус остаётся низким: он кактус, а не дерево. Но с
            // руками: столбик без рук — это столбик.
            s.trunkHeight = 3 + (rng % 3);
            s.trunkBlock = CACTUS;
            s.leafBlock = AIR;
            s.kind = 3;
            break;
        case TreeType::Dead:
            s.trunkHeight = 6 + (rng % 5);
            s.trunkBlock = WOOD;
            s.leafBlock = AIR;
            s.kind = 4;
            break;
        default: return {};
    }
    return s;
}

// Кроны кладутся в МИРОВЫХ координатах и обрезаются по своему чанку.
//
// Раньше дерево жило строго в своём чанке: анкер выбирался в нём, и
// крона резалась о границу. При кроне радиусом два это было «изредка
// обрезанные ветви», при радиусе пять — половина дерева, срезанная по
// линейке. Теперь каждый чанк доращивает и деревья соседей: раскладка
// детерминирована, и сосед строит ровно ту же крону, что и хозяин.

/// Восемь сторон света. Ветви расходятся по ним, а не куда попало:
/// случайное направление на решётке вокселей даёт не ветку, а осыпь.
static const i32 DIR8_X[8] = {  1,  1,  0, -1, -1, -1,  0,  1 };
static const i32 DIR8_Z[8] = {  0,  1,  1,  1,  0, -1, -1, -1 };

static void stampBlob(Chunk& c, i32 bx, i32 by, i32 bz, u16 leaf, i32 r,
                      u32 rng = 0)
{
    // Шар, сплюснутый сверху: вытянутый вверх выглядит как столб, а
    // идеальный — как шарик на палочке.
    //
    // Край изъеден: ровная сфера читается как ёлочный шар. Решает
    // это хэш по координате — тот же у всех, кто достраивает эту
    // крону из соседнего чанка.
    for (i32 dy = -r; dy <= r - 1; ++dy) {
        for (i32 dx = -r; dx <= r; ++dx) {
            for (i32 dz = -r; dz <= r; ++dz) {
                const i32 d2 = dx * dx + dz * dz + dy * dy * 2;
                if (d2 > r * r + r) continue;
                // Выгрызаем только внешний слой: дыра в середине
                // кроны — это дыра, а не лёгкость.
                if (d2 > r * r - r) {
                    const u32 h = hashXZ(bx + dx * 7 + dy * 13, bz + dz * 11,
                                         (u64)rng ^ 0x1EAFu);
                    if ((h & 3u) == 0u) continue;
                }
                putWorld(c, bx + dx, by + dy, bz + dz, leaf, false);
            }
        }
    }
}

/// Ветвь: наклонный ход из ствола наружу и вверх.
///
/// Ствол без ветвей — это столб, а крона на нём — шапка. Ветвь и
/// делает дерево деревом: по ней видно, что крона на чём-то держится.
static void stampBranch(Chunk& c, i32 bx, i32 by, i32 bz,
                        i32 dirX, i32 dirZ, i32 len, u16 wood)
{
    i32 x = bx, y = by, z = bz;
    for (i32 i = 0; i < len; ++i) {
        x += dirX;
        z += dirZ;
        // Через шаг вверх: ветвь идёт наклонно, а не горизонтально.
        if (i % 2 == 0) ++y;
        putWorld(c, x, y, z, wood, true);
    }
}

static void stampOak(Chunk& c, i32 bx, i32 groundY, i32 bz, const TreeShape& s) {
    const bool thick = (s.trunkHeight >= 9);
    const i32 topY = groundY + s.trunkHeight;

    // ---- Корни ----
    //
    // Четыре блока у основания. Дерево, выходящее из земли ровным
    // столбом, стоит на ней как воткнутое; с корнями — растёт.
    for (i32 d = 0; d < 4; ++d) {
        const i32 k = (i32)((s.rng >> (d * 3)) & 1u);
        if (!k) continue;
        putWorld(c, bx + DIR8_X[d * 2], groundY, bz + DIR8_Z[d * 2],
                 s.trunkBlock, false);
    }

    // ---- Ствол ----
    for (i32 dy = 0; dy < s.trunkHeight; ++dy) {
        putWorld(c, bx, groundY + dy, bz, s.trunkBlock, true);
        if (!thick) continue;
        putWorld(c, bx + 1, groundY + dy, bz,     s.trunkBlock, true);
        putWorld(c, bx,     groundY + dy, bz + 1, s.trunkBlock, true);
        putWorld(c, bx + 1, groundY + dy, bz + 1, s.trunkBlock, true);
    }

    // ---- Ветви ----
    //
    // Три-четыре, из верхней трети ствола, в разные стороны и с
    // шапкой листвы на конце. Именно концы ветвей и делают крону
    // неровной.
    const i32 count = 3 + (i32)((s.rng >> 11) & 1u);
    const i32 from = groundY + s.trunkHeight * 2 / 3;
    for (i32 i = 0; i < count; ++i) {
        const i32 dir = (i32)(((s.rng >> (i * 5 + 3)) + (u32)i * 3u) & 7u);
        const i32 at  = from + (i32)((s.rng >> (i * 4 + 17)) % 3u);
        const i32 len = 2 + (i32)((s.rng >> (i * 3 + 7)) % 3u);
        stampBranch(c, bx, at, bz, DIR8_X[dir], DIR8_Z[dir], len, s.trunkBlock);
        stampBlob(c, bx + DIR8_X[dir] * len, at + len / 2 + 1,
                  bz + DIR8_Z[dir] * len, s.leafBlock, 2, s.rng + (u32)i);
    }

    // ---- Крона ----
    //
    // Два пятна со смещением, а не одно: правильный шар на палке —
    // это гриб, и именно так дерево и выглядело.
    stampBlob(c, bx, topY, bz, s.leafBlock, thick ? 4 : 3, s.rng);
    const i32 offDir = (i32)((s.rng >> 21) & 7u);
    stampBlob(c, bx + DIR8_X[offDir], topY - 2, bz + DIR8_Z[offDir],
              s.leafBlock, thick ? 4 : 3, s.rng ^ 0x5Au);
}

/// Ель ярусами: кольца лапника с просветами между ними.
///
/// Сплошной конус — это ёлка с новогодней открытки: у настоящей
/// видно ствол между ярусами, и снизу она шире, чем кажется.
static void stampPine(Chunk& c, i32 bx, i32 groundY, i32 bz, const TreeShape& s) {
    for (i32 dy = 0; dy < s.trunkHeight; ++dy)
        putWorld(c, bx, groundY + dy, bz, s.trunkBlock, true);

    const i32 topY = groundY + s.trunkHeight;
    const i32 lowest = groundY + 2 + (i32)(s.rng % 2u);

    for (i32 y = topY; y >= lowest; --y) {
        const i32 fromTop = topY - y;
        // Ярусы через два: между ними виден ствол.
        if (fromTop % 3 == 2) continue;
        i32 r = (fromTop + 2) / 3;
        if (r > 4) r = 4;
        // Нижние лапы шире и рваные по краю.
        for (i32 dx = -r; dx <= r; ++dx)
            for (i32 dz = -r; dz <= r; ++dz) {
                const i32 d = std::abs(dx) + std::abs(dz);
                if (d > r + 1) continue;
                if (d == r + 1) {
                    const u32 h = hashXZ(bx + dx, bz + dz + y * 31,
                                         (u64)s.rng ^ 0x9E17u);
                    if ((h & 1u) == 0u) continue;
                }
                putWorld(c, bx + dx, y, bz + dz, s.leafBlock, false);
            }
    }
    // Макушка.
    putWorld(c, bx, topY + 1, bz, s.leafBlock, false);
}

/// Пальма: ствол с наклоном и повисшие листья.
static void stampPalm(Chunk& c, i32 bx, i32 groundY, i32 bz, const TreeShape& s) {
    const i32 lean = (i32)((s.rng >> 4) & 7u);
    i32 x = bx, z = bz;
    for (i32 dy = 0; dy < s.trunkHeight; ++dy) {
        // Гнётся к верхушке: прямая пальма выглядит телеграфным
        // столбом с веником.
        if (dy > s.trunkHeight / 2 && dy % 3 == 0) {
            x += DIR8_X[lean];
            z += DIR8_Z[lean];
        }
        putWorld(c, x, groundY + dy, z, s.trunkBlock, true);
    }

    const i32 topY = groundY + s.trunkHeight;
    // Листья расходятся по восьми сторонам и ОПУСКАЮТСЯ к концу.
    for (i32 d = 0; d < 8; ++d) {
        i32 lx = x, lz = z, ly = topY;
        for (i32 i = 0; i < 3; ++i) {
            lx += DIR8_X[d];
            lz += DIR8_Z[d];
            if (i == 2) --ly;          // конец листа повис
            putWorld(c, lx, ly, lz, s.leafBlock, false);
        }
    }
    putWorld(c, x, topY + 1, z, s.leafBlock, false);
}

/// Сухое дерево: ветви есть, листвы нет.
///
/// Раньше это был голый столб — ни одной ветви, ни одного листа.
/// Столб посреди саванны читается как забытый забор, а не как
/// дерево, и в Чёрном лесу таких столбов стояло по четырнадцать на
/// чанк.
static void stampDead(Chunk& c, i32 bx, i32 groundY, i32 bz, const TreeShape& s) {
    for (i32 dy = 0; dy < s.trunkHeight; ++dy)
        putWorld(c, bx, groundY + dy, bz, s.trunkBlock, true);

    // Четыре-пять кривых сучьев из верхней половины.
    const i32 count = 4 + (i32)((s.rng >> 9) & 1u);
    const i32 from = groundY + s.trunkHeight / 2;
    for (i32 i = 0; i < count; ++i) {
        const i32 dir = (i32)(((s.rng >> (i * 5)) + (u32)i * 5u) & 7u);
        const i32 at  = from + (i32)((s.rng >> (i * 4 + 13)) %
                                     (u32)std::max(1, s.trunkHeight / 2));
        const i32 len = 2 + (i32)((s.rng >> (i * 3 + 19)) % 2u);
        stampBranch(c, bx, at, bz, DIR8_X[dir], DIR8_Z[dir], len, s.trunkBlock);
    }
    // Развилка на верхушке: у сухого дерева она всегда раздвоена.
    putWorld(c, bx + 1, groundY + s.trunkHeight, bz, s.trunkBlock, false);
    putWorld(c, bx - 1, groundY + s.trunkHeight, bz, s.trunkBlock, false);
}

/// Кактус с руками.
static void stampCactus(Chunk& c, i32 bx, i32 groundY, i32 bz,
                        const TreeShape& s)
{
    for (i32 dy = 0; dy < s.trunkHeight; ++dy)
        putWorld(c, bx, groundY + dy, bz, s.trunkBlock, true);

    // Одна-две руки: вбок и сразу вверх. Без них кактус — столбик.
    const i32 arms = 1 + (i32)((s.rng >> 6) & 1u);
    for (i32 i = 0; i < arms; ++i) {
        const i32 dir = (i32)(((s.rng >> (i * 4 + 2)) & 3u) * 2u);
        const i32 at  = groundY + 1 + (i32)((s.rng >> (i * 3 + 9)) %
                                            (u32)std::max(1, s.trunkHeight - 1));
        const i32 ax = bx + DIR8_X[dir], az = bz + DIR8_Z[dir];
        putWorld(c, ax, at, az, s.trunkBlock, false);
        putWorld(c, ax, at + 1, az, s.trunkBlock, false);
        putWorld(c, ax, at + 2, az, s.trunkBlock, false);
    }
}

static void stampTree(Chunk& c, i32 bx, i32 groundY, i32 bz, const TreeShape& s) {
    switch (s.kind) {
        case 0: stampOak(c, bx, groundY, bz, s);    break;
        case 1: stampPine(c, bx, groundY, bz, s);   break;
        case 2: stampPalm(c, bx, groundY, bz, s);   break;
        case 3: stampCactus(c, bx, groundY, bz, s); break;
        case 4: stampDead(c, bx, groundY, bz, s);   break;
        default: break;
    }
}

/// Куст: шапка листвы прямо на земле, без ствола.
static void stampBush(Chunk& c, i32 bx, i32 groundY, i32 bz, u32 rng) {
    const i32 r = 1 + (i32)(rng & 1);
    for (i32 dy = 0; dy <= r; ++dy)
        for (i32 dx = -r; dx <= r; ++dx)
            for (i32 dz = -r; dz <= r; ++dz) {
                if (dx * dx + dz * dz + dy * dy * 2 > r * r + r) continue;
                putWorld(c, bx + dx, groundY + dy, bz + dz, LEAVES, false);
            }
}

} // namespace trees

/// Половина ширины дороги. Три блока: по двое разойтись, но не
/// проспект. Объявлена здесь, потому что её спрашивают и укладка
/// дороги, и отказ сажать деревья на ней.
constexpr i32 ROAD_HALF = 1;

namespace {

/// Стоит ли на этом месте постройка или дорога.
///
/// Определение ниже, рядом с раскладкой структур: она объявлена
/// дальше по тексту, а нужна здесь.
bool structureBlocks(i32 wx, i32 wz, u64 seed);
bool roadBlocks(i32 wx, i32 wz, u64 seed, const TerrainGenerator* terrain);
bool castleBlocks(i32 wx, i32 wz, u64 seed, const TerrainGenerator* terrain);
} // namespace

static bool castleCandidate(i32 superX, i32 superZ, u64 worldSeed,
                            i32& outX, i32& outZ);

namespace {

/// Деревья и кусты ОДНОГО чанка-источника, положенные в чанк c.
///
/// Источником может быть сосед: крона теперь шире чанка, и дерево,
/// выросшее рядом, обязано дотянуться ветвями сюда. Раскладка
/// детерминирована по (координата чанка, seed), поэтому сосед строит
/// ровно то же дерево, что и хозяин, — двойных стволов не выходит.
void growChunk(Chunk& c, const FeatureContext& ctx, i32 scx, i32 scz) {
    const auto& terrain = *ctx.terrain;
    const bool own = (scx == c.coord.x && scz == c.coord.z);

    const i32 midX = scx * CHUNK_SIZE + CHUNK_SIZE / 2;
    const i32 midZ = scz * CHUNK_SIZE + CHUNK_SIZE / 2;
    const auto midCol = own ? ctx.columnAt(CHUNK_SIZE / 2, CHUNK_SIZE / 2, midX, midZ)
                            : terrain.column(midX, midZ);
    const BiomeDef& biome = terrain.field().def(midCol.climate.biome);

    if (biome.treeType == TreeType::None || biome.treeDensity <= 0.01f) return;

    auto columnAt = [&](i32 lx, i32 lz, i32 wx, i32 wz) {
        return own ? ctx.columnAt(lx, lz, wx, wz) : terrain.column(wx, wz);
    };

    // ---- Деревья ----
    f32 d = biome.treeDensity;
    i32 N = (i32)d;
    const f32 frac = d - (f32)N;
    const u32 h = hashXZ(scx, scz, ctx.seed ^ 0x7EE5);
    if ((f32)(h & 0xFFFF) / 65536.f < frac) ++N;

    for (i32 i = 0; i < N; ++i) {
        const u32 rng = hashXZ(scx * 31 + i, scz * 17 + i, ctx.seed ^ 0xA11CE);
        const i32 lx = (i32)(rng & 0x1F);
        const i32 lz = (i32)((rng >> 5) & 0x1F);

        const i32 wx = scx * CHUNK_SIZE + lx;
        const i32 wz = scz * CHUNK_SIZE + lz;
        const i32 surface = columnAt(lx, lz, wx, wz).surface;
        if (surface >= CHUNK_SIZE_Y - 24) continue;
        if (surface <= TerrainGenerator::SEA_LEVEL + 1) continue;
        if (structureBlocks(wx, wz, ctx.seed)) continue;
        if (roadBlocks(wx, wz, ctx.seed, ctx.terrain)) continue;
        if (castleBlocks(wx, wz, ctx.seed, ctx.terrain)) continue;

        const trees::TreeShape shape = trees::treeShapeFor(biome.treeType, rng);
        if (shape.trunkHeight == 0) continue;
        trees::stampTree(c, wx, surface, wz, shape);
    }

    // ---- Кусты ----
    //
    // Плотность выводится из древесной, а не заводится своей колонкой
    // в таблице биомов: две колонки об одном и том же расходятся
    // молча, а куст — это подлесок, и растёт он там же, где лес.
    const i32 bushes = (i32)(biome.treeDensity * 3.f);
    for (i32 i = 0; i < bushes; ++i) {
        const u32 rng = hashXZ(scx * 13 + i, scz * 29 + i, ctx.seed ^ 0xB005);
        const i32 lx = (i32)(rng & 0x1F);
        const i32 lz = (i32)((rng >> 5) & 0x1F);

        const i32 wx = scx * CHUNK_SIZE + lx;
        const i32 wz = scz * CHUNK_SIZE + lz;
        const i32 surface = columnAt(lx, lz, wx, wz).surface;
        if (surface >= CHUNK_SIZE_Y - 8) continue;
        if (surface <= TerrainGenerator::SEA_LEVEL + 1) continue;
        if (structureBlocks(wx, wz, ctx.seed)) continue;
        if (roadBlocks(wx, wz, ctx.seed, ctx.terrain)) continue;
        if (castleBlocks(wx, wz, ctx.seed, ctx.terrain)) continue;
        trees::stampBush(c, wx, surface, wz, rng >> 10);
    }
}

} // namespace

void applyTrees(Chunk& chunk, const FeatureContext& ctx) {
    // Свой чанк и восемь соседних: крона большого дерева шире чанка.
    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx)
            growChunk(chunk, ctx, chunk.coord.x + dx, chunk.coord.z + dz);
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
// РЕКИ
//
// RiverGenerator строит мировую сеть независимо от границ чанков.
// Здесь сеть превращается в непрерывное воксельное русло: сегменты
// интерполируются каждые ~2 блока, waterY спускается вместе с
// рельефом, а ширина растёт по пути к устью.
// ============================================================

void applyRivers(Chunk& chunk, const FeatureContext& ctx) {
    if (!ctx.terrain) return;

    std::vector<std::shared_ptr<const TerrainGenerator::RiverNetwork>> nets;
    ctx.terrain->riverNetworksNear(chunk.coord.x, chunk.coord.z, nets);
    if (nets.empty()) return;

    std::array<i16, CHUNK_SIZE * CHUNK_SIZE> waterTop;
    std::array<f32, CHUNK_SIZE * CHUNK_SIZE> waterWidth;
    waterTop.fill((i16)-1);
    waterWidth.fill(0.f);

    const i32 bx0 = chunk.coord.x * CHUNK_SIZE;
    const i32 bz0 = chunk.coord.z * CHUNK_SIZE;

    auto markSample = [&](f32 fx, f32 fz, f32 fy, f32 width) {
        const i32 cx = (i32)std::lround(fx);
        const i32 cz = (i32)std::lround(fz);
        const i32 radius = std::max(1, (i32)std::ceil(width + 0.75f));

        for (i32 dz = -radius; dz <= radius; ++dz) {
            for (i32 dx = -radius; dx <= radius; ++dx) {
                const f32 ddx = (f32)dx;
                const f32 ddz = (f32)dz;
                if (ddx * ddx + ddz * ddz > width * width) continue;

                const i32 wx = cx + dx;
                const i32 wz = cz + dz;
                const i32 lx = wx - bx0;
                const i32 lz = wz - bz0;
                if ((u32)lx >= (u32)CHUNK_SIZE ||
                    (u32)lz >= (u32)CHUNK_SIZE) continue;

                const usize k = (usize)lx * CHUNK_SIZE + lz;
                const i16 top = (i16)std::clamp(
                    (i32)std::lround(fy), 1, CHUNK_SIZE_Y - 2);

                // На слиянии рек более низкая отметка побеждает: приток
                // не может образовать полку выше основного русла.
                if (waterTop[k] < 0 || top < waterTop[k]) {
                    waterTop[k] = top;
                    waterWidth[k] = width;
                } else if (top == waterTop[k] && width > waterWidth[k]) {
                    waterWidth[k] = width;
                }
            }
        }
    };

    for (const auto& net : nets) {
        if (!net) continue;
        for (const auto& path : net->paths) {
            if (path.points.empty()) continue;

            if (path.points.size() == 1) {
                const auto& p = path.points.front();
                markSample((f32)p.x, (f32)p.z, (f32)p.waterY, p.width);
                continue;
            }

            for (usize i = 0; i + 1 < path.points.size(); ++i) {
                const auto& a = path.points[i];
                const auto& b = path.points[i + 1];

                const f32 dx = (f32)b.x - (f32)a.x;
                const f32 dz = (f32)b.z - (f32)a.z;
                const f32 len = std::hypot(dx, dz);
                const i32 steps = std::max(1, (i32)std::ceil(len / 2.f));

                for (i32 s = 0; s <= steps; ++s) {
                    const f32 t = (f32)s / (f32)steps;
                    const f32 x = (f32)a.x + dx * t;
                    const f32 z = (f32)a.z + dz * t;
                    const f32 y = (f32)a.waterY +
                                  ((f32)b.waterY - (f32)a.waterY) * t;
                    const f32 w = a.width + (b.width - a.width) * t;
                    markSample(x, z, y, w);
                }
            }
        }
    }

    // Одно применение к вокселям вместо перезаписи одной и той же
    // колонки сотни раз при прохождении длинного русла через чанк.
    for (i32 lx = 0; lx < CHUNK_SIZE; ++lx) {
        for (i32 lz = 0; lz < CHUNK_SIZE; ++lz) {
            const usize k = (usize)lx * CHUNK_SIZE + lz;
            if (waterTop[k] < 0 || waterWidth[k] <= 0.f) continue;

            const i32 wx = bx0 + lx;
            const i32 wz = bz0 + lz;
            const i32 surface = ctx.columnAt(lx, lz, wx, wz).surface;

            // Под общим уровнем моря applyLiquids уже залил воду. Устье
            // не должно вырезать из моря собственный более низкий уровень.
            if (surface <= TerrainGenerator::SEA_LEVEL) continue;

            const i32 top = std::min((i32)waterTop[k], surface);
            if (top < 2) continue;

            const u32 h = feat_util::hashXZ(wx, wz, ctx.seed ^ 0x51F3E7ULL);
            i32 depth = 2 + (i32)std::floor(waterWidth[k] * 0.35f)
                        + (i32)(h & 1u);
            depth = std::clamp(depth, 2, 6);

            const i32 bed = std::max(1, top - depth);

            // Вода заполняет русло от дна до поверхности. Всё выше
            // поверхности до старой земли вырезается.
            for (i32 y = bed; y < surface; ++y) {
                chunk.voxels[chunkIndex(lx, y, lz)] =
                    y <= top ? WATER : AIR;
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
    TreeDungeon,   ///< подземелье внутри исполинского дерева
};

// Описание структуры: AABB в блоках, тип, seed
struct Layout {
    Kind kind = None;
    glm::ivec3 minBlock{0};
    glm::ivec3 maxBlock{0};
    u32  seed = 0;
};

// Ячейки сетки, в которых начинается игра.
//
// Перебор точки появления идёт по области 100x100 вокруг начала
// координат, а сторона ячейки — 256 блоков. Значит область задевает
// ровно четыре ячейки: те, что сходятся углами в нуле.
bool isHomeCell(i32 sx, i32 sz) {
    return (sx == 0 || sx == -1) && (sz == 0 || sz == -1);
}

// Детерминированно выбираем структуру для super-chunk (sx, sz).
Layout layoutFor(i32 sx, i32 sz, u64 worldSeed) {
    Layout L;
    u32 h = hashXZ(sx, sz, worldSeed ^ 0x517);
    // Расположение внутри super-chunk: смещение от 0..100, размер ≤156
    u32 sel = h & 0xFF;

    // ---- Начало пути ----
    //
    // У четырёх ячеек вокруг нуля выбор короче: деревня или ничего.
    // Подземелье, руина, алтарь и дерево-подземелье — места, куда
    // ХОДЯТ, а не места, где просыпаются: в каждом из них твари, и
    // стоят они ровно там, где игрок делает первые шаги.
    //
    // Деревня, наоборот, — лучшее, что может случиться с началом
    // игры: торговец, квесты, кровати и стража. Поэтому она
    // разрешена, и только в ближней ячейке: в дальних её было бы не
    // видно за туманом, и толку от неё столько же, сколько от любой
    // другой деревни на карте.
    if (isHomeCell(sx, sz)) {
        const bool nearest = (sx == 0 && sz == 0);
        // Свой хэш: структурный уже потрачен на выбор из шести, и
        // «деревня» в нём — каждый пятый. Здесь нужна половина.
        const u32 hv = hashXZ(sx, sz, worldSeed ^ 0xD0E1ULL);
        if (!nearest || (hv & 1u) == 0u) return L;
        L.kind = Village;
    }
    // 20% деревня, 15% руины, 25% подземелье, 8% алтарь,
    // 9% дерево-подземелье, иначе пусто.
    else if (sel < 51)  L.kind = Village;
    else if (sel < 90)  L.kind = Ruin;
    else if (sel < 154) L.kind = Dungeon;
    else if (sel < 174) L.kind = Altar;
    else if (sel < 197) L.kind = TreeDungeon;
    else return L;

    i32 baseX = sx * SUPER_BLOCKS;
    i32 baseZ = sz * SUPER_BLOCKS;
    i32 ox = (i32)((h >> 8) & 0x3F);     // 0..63
    i32 oz = (i32)((h >> 14) & 0x3F);

    // Размер зависит от типа
    i32 halfX = (L.kind == Village) ? 48 : (L.kind == Dungeon) ? 56
              : (L.kind == TreeDungeon) ? 20 : 12;
    i32 halfZ = halfX;
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

namespace {

/// Лес не растёт сквозь постройки.
///
/// Деревья кладутся ПОСЛЕ структур, и с прежней плотностью в полтора
/// дерева на чанк это почти не встречалось. С семью — дуб вырастает
/// посреди деревенской мостовой, и дорожка под ним пропадает.
///
/// Запас в четыре блока: крона шире ствола, и дерево вплотную к стене
/// накрывает крышу.
bool structureBlocks(i32 wx, i32 wz, u64 seed) {
    constexpr i32 MARGIN = 4;
    const i32 sx = (i32)std::floor((f32)wx / (f32)structs::SUPER_BLOCKS);
    const i32 sz = (i32)std::floor((f32)wz / (f32)structs::SUPER_BLOCKS);
    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const structs::Layout L = structs::layoutFor(sx + dx, sz + dz, seed);
            if (L.kind == structs::None) continue;
            if (wx < L.minBlock.x - MARGIN || wx > L.maxBlock.x + MARGIN) continue;
            if (wz < L.minBlock.z - MARGIN || wz > L.maxBlock.z + MARGIN) continue;
            return true;
        }
    return false;
}

/// Лес не растёт сквозь замок.
///
/// structureBlocks смотрит на сетку структур, а замка в ней нет: он
/// решается своим хэшем и биомом. Без отдельной проверки Чёрный лес
/// с его четырнадцатью деревьями на чанк зарастил бы двор насквозь.
bool castleBlocks(i32 wx, i32 wz, u64 seed, const TerrainGenerator* terrain) {
    constexpr i32 KEEP = CASTLE_HALF + 4;
    const i32 sc0x = (i32)std::floor((f32)wx / (f32)structs::SUPER_BLOCKS);
    const i32 sc0z = (i32)std::floor((f32)wz / (f32)structs::SUPER_BLOCKS);
    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            // Сперва дешёвое: где замок стоял бы и близко ли это.
            // Дорогой вопрос «а Чёрный ли тут лес» задаётся только
            // тем ячейкам, чей замок и правда накрыл бы эту точку.
            i32 cx = 0, cz = 0;
            if (!castleCandidate(sc0x + dx, sc0z + dz, seed, cx, cz)) continue;
            if (std::abs(wx - cx) > KEEP || std::abs(wz - cz) > KEEP) continue;
            if (castleAt(sc0x + dx, sc0z + dz, seed, terrain).exists) return true;
        }
    return false;
}

/// Лес не растёт на дороге.
///
/// Дорога кладётся ДО деревьев, и дуб, выросший посреди неё, дорогу
/// не только закрывает — он ещё и обрывает путь, по которому ходят
/// посыльные.
bool roadBlocks(i32 wx, i32 wz, u64 seed, const TerrainGenerator* terrain) {
    // Три блока от оси — полотно шириной в три плюс блок с каждой
    // стороны. Этого хватает и стволу, и кроне: над дорогой ветви
    // проходят уже выше человеческого роста.
    constexpr f32 KEEP = (f32)ROAD_HALF + 2.f;

    auto nearSegment = [&](const glm::ivec3& a, const glm::ivec3& b) {
        const f32 ax = (f32)a.x, az = (f32)a.z;
        const f32 vx = (f32)(b.x - a.x), vz = (f32)(b.z - a.z);
        const f32 len2 = vx * vx + vz * vz;
        if (len2 < 1e-3f) return false;
        f32 t = (((f32)wx - ax) * vx + ((f32)wz - az) * vz) / len2;
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        const f32 dx = (f32)wx - (ax + vx * t);
        const f32 dz = (f32)wz - (az + vz * t);
        return dx * dx + dz * dz <= KEEP * KEEP;
    };

    const i32 sc0x = (i32)std::floor((f32)wx / (f32)structs::SUPER_BLOCKS);
    const i32 sc0z = (i32)std::floor((f32)wz / (f32)structs::SUPER_BLOCKS);
    for (i32 dz = -2; dz <= 2; ++dz)
        for (i32 dx = -2; dx <= 2; ++dx) {
            const i32 sx = sc0x + dx, sz = sc0z + dz;
            const VillageSite a = villageAt(sx, sz, seed, terrain);
            if (!a.exists) continue;
            const VillageSite e = villageAt(sx + 1, sz, seed, terrain);
            if (e.exists && nearSegment(a.center, e.center)) return true;
            const VillageSite s2 = villageAt(sx, sz + 1, seed, terrain);
            if (s2.exists && nearSegment(a.center, s2.center)) return true;
        }
    return false;
}

} // namespace

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
/// Из чего сложен дом. Не украшение: уклад деревни видно с порога
/// именно по материалу стены и кровли.
using HouseKit = VillageMaterials;

void stampHouse(Chunk& c, const FeatureContext& ctx,
                i32 wx, i32 wz, i32 w, i32 d, u32 rng, Facing face,
                const HouseKit& kit)
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
                putWorld(c, gx, surf + y, gz, corner ? kit.post : kit.wall, true);
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
                        putWorld(c, wx + t, y, gz, kit.roof, true);
                } else {
                    putWorld(c, wx + lo, y, gz, kit.roof, true);
                    putWorld(c, wx + hi, y, gz, kit.roof, true);
                    // Под скатом — воздух, иначе чердак зальётся соломой.
                    for (i32 t = lo + 1; t < hi; ++t)
                        putWorld(c, wx + t, y, gz, AIR, true);
                }
            }
        } else {
            for (i32 gx = wx - 1; gx <= wx + w; ++gx) {
                if (closing) {
                    for (i32 t = lo; t <= hi; ++t)
                        putWorld(c, gx, y, wz + t, kit.roof, true);
                } else {
                    putWorld(c, gx, y, wz + lo, kit.roof, true);
                    putWorld(c, gx, y, wz + hi, kit.roof, true);
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

/// Набор материалов и мерки кольца домов по укладу.
///
/// Всё, чем деревни отличаются друг от друга ВНЕШНЕ, собрано в одном
/// месте: разложи это по коду стройки — и добавить пятый уклад можно
/// будет только вычитав всю функцию.
struct VillagePlan {
    HouseKit kit;
    f32 ringBase;    ///< на каком радиусе стоит первый дом
    u32 ringJitter;  ///< насколько радиус гуляет от дома к дому
    i32 extraHouses; ///< домов больше или меньше обычного
};

VillagePlan villagePlanFor(VillageStyle st) {
    const VillageMaterials kit = villageMaterialsOf(st);
    switch (st) {
        // Кольцо тесное — ремесленники жмутся к мастерским.
        case VillageStyle::Stonemason: return { kit, 16.f, 0x7,  2 };
        // Ещё плотнее: такую деревню держат как заставу, а не хутор.
        case VillageStyle::Garrison:   return { kit, 14.f, 0x3,  0 };
        // Дома вразброс: ровного кольца в лесу не выходит.
        case VillageStyle::Woodland:   return { kit, 24.f, 0x1F, -1 };
        // Просторное кольцо. Такой деревня была всегда — она и
        // осталась одной из.
        default:                       return { kit, 20.f, 0xF,  0 };
    }
}

void stampVillage(Chunk& c, const FeatureContext& ctx, const structs::Layout& L) {
    const i32 cx = (L.minBlock.x + L.maxBlock.x) / 2;
    const i32 cz = (L.minBlock.z + L.maxBlock.z) / 2;
    const i32 wy = ctx.terrain->surfaceHeight(cx, cz);

    const u32 h = L.seed;
    const VillagePlan plan = villagePlanFor(villageStyleOf(h));
    const i32 count = std::max(4, 6 + (i32)(h % 5) + plan.extraHouses);

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
        const f32 r = plan.ringBase + (f32)((h >> (i % 16)) & plan.ringJitter);

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
        stampHouse(c, ctx, p.x, p.z, p.w, p.d, h + (u32)i * 31u, p.face,
                   plan.kit);
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

// ============================================================
// Подземелье внутри исполинского дерева
// ============================================================
//
// Ствол полый: стена в два блока, внутри шахта, по стене поднимается
// лестница, этажи её пересекают.
//
// Лестница идёт по КВАДРАТНОМУ кольцу, а не по спирали из синуса с
// косинусом, и это не украшение. Отдельного блока-лестницы в игре
// нет: подъём делает контроллер, и делает его на один блок за шаг.
// Значит ступени обязаны быть соседними по стороне — по диагонали
// тело шириной 0.8 между двумя углами не проходит. Обход квадрата
// даёт соседство по стороне по построению; круг, округлённый до
// целых, — нет, там через раз выходит диагональ.
//
// Из того же обхода берётся и запас над головой. Кольцо в 24 клетки,
// подъём на блок за клетку: одна и та же клетка занята ступенями,
// отстоящими на 24 блока по высоте. Игроку нужно два — с запасом.
// Считать высоту и проверять её отдельно не нужно вовсе.
namespace treedung {

constexpr i32 R_OUT      = 7;    ///< внешний радиус ствола
constexpr i32 R_IN       = 5;    ///< внутренний
constexpr i32 HEIGHT     = 46;   ///< высота ствола над землёй
constexpr i32 FLOOR_STEP = 9;    ///< через сколько блоков этаж
constexpr i32 TOP_ROOM   = 7;    ///< высота верхнего зала

/// Радиус лестничного кольца.
///
/// Три, а не четыре: углы кольца отстоят от середины на sqrt(2)*R, и
/// при четырёх это 5.66 — дальше внутренней стены. Ступени в углах
/// оказались бы замурованы в дереве.
constexpr i32 STAIR_R    = 3;
constexpr i32 RING_LEN   = 8 * STAIR_R;   ///< клеток в кольце

/// k-я клетка обхода кольца, смещением от середины.
inline void ringCell(i32 k, i32& dx, i32& dz) {
    const i32 R = STAIR_R, side = 2 * R;
    k = ((k % RING_LEN) + RING_LEN) % RING_LEN;
    if      (k < side)     { dx =  R;                dz = -R + k; }
    else if (k < 2 * side) { dx =  R - (k - side);   dz =  R; }
    else if (k < 3 * side) { dx = -R;                dz =  R - (k - 2 * side); }
    else                   { dx = -R + (k - 3 * side); dz = -R; }
}

/// Высота верхнего зала над землёй — там же стоит босс.
inline i32 topFloorDy() { return HEIGHT - TOP_ROOM; }

/// Попадает ли клетка в проём перекрытия на высоте dy.
///
/// Вырез идёт по трём ступеням — той, что на уровне этажа, и двум под
/// ней: через них игрок и проходит сквозь перекрытие. Правило одно на
/// все перекрытия, включая пол верхнего зала: там оно однажды и
/// разошлось, зал накрыл голову поднимающемуся, и подъём обрывался за
/// три блока до конца.
inline bool floorHole(i32 dy, i32 dx, i32 dz) {
    for (i32 k = 0; k < 3; ++k) {
        i32 hx, hz;
        ringCell(dy - k, hx, hz);
        if (std::abs(dx - hx) <= 2 && std::abs(dz - hz) <= 2) return true;
    }
    return false;
}

/// Годится ли рельеф под дерево. Условие одно на всех, кто спрашивает.
inline bool groundFits(i32 ground) {
    return ground >= 6 && ground <= CHUNK_SIZE_Y - HEIGHT - 16;
}

} // namespace treedung

void stampTreeDungeon(Chunk& c, const FeatureContext& ctx,
                      const structs::Layout& L)
{
    using namespace treedung;

    const i32 cx = (L.minBlock.x + L.maxBlock.x) / 2;
    const i32 cz = (L.minBlock.z + L.maxBlock.z) / 2;
    const i32 ground = ctx.terrain->surfaceHeight(cx, cz);
    if (!groundFits(ground)) return;

    // Столбцы обходим ТОЛЬКО в пределах своего чанка. Дерево шире
    // чанка, и без этого каждый из задетых чанков перебирал бы весь
    // ствол целиком — кратная работа на ровном месте.
    const i32 bx0 = c.coord.x * CHUNK_SIZE, bx1 = bx0 + CHUNK_SIZE - 1;
    const i32 bz0 = c.coord.z * CHUNK_SIZE, bz1 = bz0 + CHUNK_SIZE - 1;
    const i32 x0 = std::max(cx - 12, bx0), x1 = std::min(cx + 12, bx1);
    const i32 z0 = std::max(cz - 12, bz0), z1 = std::min(cz + 12, bz1);
    if (x0 > x1 || z0 > z1) return;

    const i32 top = ground + HEIGHT;

    // ---- Ствол и крона ----
    for (i32 wz = z0; wz <= z1; ++wz) {
        for (i32 wx = x0; wx <= x1; ++wx) {
            const i32 dx = wx - cx, dz = wz - cz;
            const i32 r2 = dx * dx + dz * dz;

            if (r2 <= 12 * 12) {
                const i32 cy = top + 4;
                for (i32 y = cy - 5; y <= cy + 5; ++y) {
                    const i32 dy = y - cy;
                    if (r2 + dy * dy * 4 > 12 * 12) continue;
                    putWorld(c, wx, y, wz, LEAVES, false);
                }
            }

            if (r2 > R_OUT * R_OUT) continue;
            const bool inside = (r2 <= R_IN * R_IN);
            for (i32 y = ground; y <= top; ++y)
                putWorld(c, wx, y, wz, inside ? AIR : WOOD, true);
            // Пол шахты: у входа земля может уйти вниз, и без пола
            // шахта проваливается.
            if (inside) putWorld(c, wx, ground - 1, wz, PLANK, true);
        }
    }

    // ---- Вход ----
    //
    // Со стороны +X, на всю толщину стены: без него дерево — глухой
    // столб, и подземелья внутри никто не найдёт.
    //
    // Режется ДО лестницы, а не после. Наоборот — и проём сносит
    // первые три ступени вместе с началом подъёма: они приходятся
    // ровно на его высоту и ширину.
    for (i32 wx = cx; wx <= cx + R_OUT; ++wx)
        for (i32 wz = cz - 1; wz <= cz + 1; ++wz)
            for (i32 y = ground; y <= ground + 3; ++y)
                putWorld(c, wx, y, wz, AIR, true);

    // ---- Этажи ----
    //
    // Режутся вокруг лестницы: сплошной этаж запер бы подъём. Вырез
    // идёт по трём ступеням — той, что на уровне этажа, и двум под
    // ней: через них игрок и проходит сквозь перекрытие.
    for (i32 dy = FLOOR_STEP; dy < topFloorDy(); dy += FLOOR_STEP) {
        for (i32 wz = std::max(cz - R_IN, z0); wz <= std::min(cz + R_IN, z1); ++wz)
            for (i32 wx = std::max(cx - R_IN, x0); wx <= std::min(cx + R_IN, x1); ++wx) {
                const i32 dx = wx - cx, dz = wz - cz;
                if (dx * dx + dz * dz > R_IN * R_IN) continue;
                if (floorHole(dy, dx, dz)) continue;
                putWorld(c, wx, ground + dy, wz, PLANK, true);
            }
    }

    // ---- Лестница ----
    for (i32 dy = 1; dy < topFloorDy(); ++dy) {
        i32 dx, dz;
        ringCell(dy, dx, dz);
        putWorld(c, cx + dx, ground + dy, cz + dz, PLANK, true);
        // Фонарь по столбу в середине: без света внутри ствола не
        // видно ни этажей, ни проёмов.
        if (dy % 6 == 0) putWorld(c, cx, ground + dy, cz, LANTERN, true);
    }

    // ---- Верхний зал ----
    //
    // Шире шахты: здесь стоит босс, и драться в колодце диаметром в
    // десять блоков негде.
    {
        const i32 fy = ground + topFloorDy();
        for (i32 wz = std::max(cz - R_OUT + 1, z0); wz <= std::min(cz + R_OUT - 1, z1); ++wz)
            for (i32 wx = std::max(cx - R_OUT + 1, x0); wx <= std::min(cx + R_OUT - 1, x1); ++wx) {
                const i32 dx = wx - cx, dz = wz - cz;
                if (dx * dx + dz * dz > (R_OUT - 1) * (R_OUT - 1)) continue;
                for (i32 y = fy; y < fy + TOP_ROOM; ++y)
                    putWorld(c, wx, y, wz, AIR, true);
                if (floorHole(topFloorDy() - 1, dx, dz)) continue;
                putWorld(c, wx, fy - 1, wz, PLANK, true);
            }
        putWorld(c, cx, fy + 1, cz, LANTERN, true);
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
DungeonSite treeDungeonAt(i32 superX, i32 superZ, u64 worldSeed,
                          const TerrainGenerator* terrain)
{
    DungeonSite site;
    if (!terrain) return site;
    const structs::Layout L = structs::layoutFor(superX, superZ, worldSeed);
    if (L.kind != structs::TreeDungeon) return site;

    const i32 cx = (L.minBlock.x + L.maxBlock.x) / 2;
    const i32 cz = (L.minBlock.z + L.maxBlock.z) / 2;
    const i32 ground = terrain->surfaceHeight(cx, cz);

    // Тот же отказ, что и в stampTreeDungeon: дерева здесь не выросло,
    // значит и зала нет. Две копии условия разошлись бы молча, поэтому
    // числа берутся из одного места.
    if (!treedung::groundFits(ground)) return site;

    site.exists = true;
    site.seed   = L.seed;
    // Пол зала лежит блоком НИЖЕ отметки: стоят на нём, а не в нём.
    site.center = { cx, ground + treedung::topFloorDy(), cz };
    return site;
}

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

const char* villageStyleName(VillageStyle s) {
    switch (s) {
        case VillageStyle::Farmstead:  return "Farmstead";
        case VillageStyle::Stonemason: return "Stonemason village";
        case VillageStyle::Garrison:   return "Garrison";
        case VillageStyle::Woodland:   return "Woodland hamlet";
        default:                       return "Village";
    }
}

VillageMaterials villageMaterialsOf(VillageStyle s) {
    switch (s) {
        // Каменная: тёсаная кладка, кровля из доски.
        case VillageStyle::Stonemason: return { BRICK, WOOD,  PLANK };
        // Сторожевая: кладка и кладка, без единой доски в стене.
        case VillageStyle::Garrison:   return { BRICK, BRICK, PLANK };
        // Лесная: бревенчатые стены под соломой.
        case VillageStyle::Woodland:   return { WOOD,  WOOD,  THATCH };
        // Хлебная: доска и солома.
        default:                       return { PLANK, WOOD,  THATCH };
    }
}

VillageStyle villageStyleOf(u32 villageSeed) {
    // Сдвиг на шестнадцать: младшие биты того же хэша уже разобраны
    // раскладкой (размер, смещение, число домов). Взять их ещё раз
    // значило бы связать уклад с размером деревни намертво.
    return (VillageStyle)((villageSeed >> 16) % (u32)VillageStyle::Count);
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
    site.style  = villageStyleOf(L.seed);
    // Колодец ставится ровно в середину раскладки — там же, где его
    // рисует stampVillage, и на ту же высоту.
    //
    // Высота раньше отдавалась нулём, и спрашивающий не мог узнать,
    // где колодец стоит: точка возрождения по такому ответу ушла бы
    // под мир. Без генератора её не узнать, поэтому нуль остаётся
    // только когда его не передали.
    const i32 wy = terrain ? terrain->surfaceHeight(cx, cz) : 0;
    site.center = { cx, wy, cz };
    return site;
}

// ============================================================
// Болотные бочаги
// ============================================================
//
// Болото было болотом только по имени: та же трава, тот же рельеф,
// разве что суше на два блока и деревья сухие. Стоячая вода — то
// единственное, по чему болото узнают с первого взгляда.
void applySwampPools(Chunk& chunk, const FeatureContext& ctx) {
    const auto& terrain = *ctx.terrain;
    const i32 bx0 = chunk.coord.x * CHUNK_SIZE;
    const i32 bz0 = chunk.coord.z * CHUNK_SIZE;

    // Дёшево: биом спрашивается ОДИН раз на чанк, по его середине.
    // Побочный эффект тут в пользу — бочаги кончаются на границе
    // чанка, и берег выходит рваным, как ему и положено.
    const i32 midX = bx0 + CHUNK_SIZE / 2;
    const i32 midZ = bz0 + CHUNK_SIZE / 2;
    if (terrain.biomeAt(midX, midZ) != Swamp) return;

    for (i32 lz = 0; lz < CHUNK_SIZE; ++lz)
        for (i32 lx = 0; lx < CHUNK_SIZE; ++lx) {
            const i32 wx = bx0 + lx, wz = bz0 + lz;

            // Пятна воды, а не рябь по всему болоту: хэш по клетке
            // четыре на четыре даёт бочаги, по которым можно обойти.
            const u32 h = hashXZ(wx >> 2, wz >> 2, ctx.seed ^ 0x5A4A6);
            if ((h & 0xFF) >= 96) continue;

            const i32 top = ctx.columnAt(lx, lz, wx, wz).surface;
            if (top <= TerrainGenerator::SEA_LEVEL) continue;
            if (top >= CHUNK_SIZE_Y - 4) continue;

            // Бочаг в один-два блока глубиной. Глубже — это уже
            // озеро, и в него придётся плыть, а по болоту ходят.
            const i32 depth = 1 + (i32)((h >> 8) & 1);
            for (i32 d = 0; d < depth; ++d)
                put(chunk, lx, top - 1 - d, lz, WATER);
            // Дно илистое: под водой болота не камень.
            put(chunk, lx, top - 1 - depth, lz, DIRT);
        }
}

// ============================================================
// Тайники под руинами
// ============================================================
namespace treasure {

constexpr i32 DEPTH    = 6;   ///< на сколько ниже поверхности
constexpr i32 HALF     = 2;   ///< полкамеры: 5x5
constexpr i32 HEIGHT   = 3;

} // namespace treasure

TreasureSite treasureAt(i32 superX, i32 superZ, u64 worldSeed,
                        const TerrainGenerator* terrain)
{
    TreasureSite site;
    if (!terrain) return site;

    // Клад лежит под руинами. Не потому, что так красивее, а потому
    // что клад в чистом поле не находят: его находят по рассказу, а
    // рассказывают про место, у которого есть имя.
    const structs::Layout L = structs::layoutFor(superX, superZ, worldSeed);
    if (L.kind != structs::Ruin) return site;

    const i32 cx = (L.minBlock.x + L.maxBlock.x) / 2;
    const i32 cz = (L.minBlock.z + L.maxBlock.z) / 2;
    const i32 top = terrain->surfaceHeight(cx, cz);
    if (top < TerrainGenerator::SEA_LEVEL + 4) return site;
    // Камера целиком должна помещаться выше коренной породы.
    if (top - treasure::DEPTH - treasure::HEIGHT < 4) return site;

    site.exists = true;
    site.seed   = L.seed;
    site.center = { cx, top - treasure::DEPTH, cz };
    return site;
}

TreasureSite nearestTreasure(const glm::ivec3& from, u64 worldSeed,
                             const TerrainGenerator* terrain,
                             i32 maxBlocks)
{
    TreasureSite best;
    i64 bestD2 = (i64)maxBlocks * maxBlocks;

    const i32 reach = maxBlocks / structs::SUPER_BLOCKS + 1;
    const i32 sc0x = (i32)std::floor((f32)from.x / (f32)structs::SUPER_BLOCKS);
    const i32 sc0z = (i32)std::floor((f32)from.z / (f32)structs::SUPER_BLOCKS);

    for (i32 dz = -reach; dz <= reach; ++dz)
        for (i32 dx = -reach; dx <= reach; ++dx) {
            const TreasureSite t = treasureAt(sc0x + dx, sc0z + dz,
                                              worldSeed, terrain);
            if (!t.exists) continue;
            const i64 ddx = t.center.x - from.x;
            const i64 ddz = t.center.z - from.z;
            const i64 d2 = ddx * ddx + ddz * ddz;
            if (d2 < bestD2) { bestD2 = d2; best = t; }
        }
    return best;
}

/// Камера тайника в своём чанке.
///
/// Замурована намеренно: ни хода, ни лестницы. Единственный способ
/// попасть внутрь — разобрать землю сверху, и ровно это делает клад
/// кладом, а не комнатой с сундуком.
static void applyTreasures(Chunk& c, const FeatureContext& ctx) {
    const i32 bx0 = c.coord.x * CHUNK_SIZE;
    const i32 bz0 = c.coord.z * CHUNK_SIZE;
    const i32 sc0x = (i32)std::floor((f32)bx0 / (f32)structs::SUPER_BLOCKS);
    const i32 sc0z = (i32)std::floor((f32)bz0 / (f32)structs::SUPER_BLOCKS);

    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const TreasureSite t = treasureAt(sc0x + dx, sc0z + dz,
                                              ctx.seed, ctx.terrain);
            if (!t.exists) continue;
            const i32 reach = treasure::HALF + 1;
            if (bx0 + CHUNK_SIZE - 1 < t.center.x - reach ||
                bx0 > t.center.x + reach) continue;
            if (bz0 + CHUNK_SIZE - 1 < t.center.z - reach ||
                bz0 > t.center.z + reach) continue;

            const i32 y0 = t.center.y;
            const i32 y1 = y0 + treasure::HEIGHT - 1;
            for (i32 wz = t.center.z - reach; wz <= t.center.z + reach; ++wz)
                for (i32 wx = t.center.x - reach; wx <= t.center.x + reach; ++wx) {
                    const bool wall = std::abs(wx - t.center.x) > treasure::HALF ||
                                      std::abs(wz - t.center.z) > treasure::HALF;
                    for (i32 y = y0 - 1; y <= y1 + 1; ++y) {
                        const bool cap = (y < y0 || y > y1);
                        putWorld(c, wx, y, wz, (wall || cap) ? STONE : AIR, true);
                    }
                }

            // Золотая жила по углам пола: она и видна, и означает
            // «здесь копали не зря».
            const i32 g = treasure::HALF - 1;
            putWorld(c, t.center.x - g, y0, t.center.z - g, GOLD_ORE, true);
            putWorld(c, t.center.x + g, y0, t.center.z - g, GOLD_ORE, true);
            putWorld(c, t.center.x - g, y0, t.center.z + g, GOLD_ORE, true);
            putWorld(c, t.center.x + g, y0, t.center.z + g, GOLD_ORE, true);
            // Фонарь посередине: камера без света — чёрный куб.
            putWorld(c, t.center.x, y1, t.center.z, LANTERN, true);
        }
}

// ============================================================
// Провалы
// ============================================================
// ============================================================
//
// Обрыв, в который падают. Отвесные стены, дно глубоко внизу и
// каменный венец по краю: без венца дыра в траве читается как
// ошибка генератора, а не как место.
namespace {

/// Где провал БЫЛ БЫ — по одним хэшам, без запросов к рельефу.
/// Та же причина, что и у замка: спрашивают часто, а рельеф дорог.
bool sinkholeCandidate(i32 superX, i32 superZ, u64 worldSeed,
                       i32& outX, i32& outZ, i32& outR)
{
    // Провал живёт в своей ячейке и не делит её со структурой:
    // яма посреди деревни — это не опасность, это поломка.
    if (structs::layoutFor(superX, superZ, worldSeed).kind != structs::None)
        return false;

    const u32 h = hashXZ(superX, superZ, worldSeed ^ 0x5140);
    if ((h & 0xFF) >= 110) return false;

    outX = superX * structs::SUPER_BLOCKS + 40 + (i32)((h >> 8)  & 0x7F);
    outZ = superZ * structs::SUPER_BLOCKS + 40 + (i32)((h >> 15) & 0x7F);
    // Радиус 5..10: уже пяти — колодец, шире десяти — котлован, и
    // по краю его уже обходят, а не падают.
    outR = 5 + (i32)((h >> 23) & 0x5);
    return true;
}

} // namespace

SinkholeSite sinkholeAt(i32 superX, i32 superZ, u64 worldSeed,
                        const TerrainGenerator* terrain)
{
    SinkholeSite site;
    if (!terrain) return site;

    i32 cx = 0, cz = 0, r = 0;
    if (!sinkholeCandidate(superX, superZ, worldSeed, cx, cz, r)) return site;

    // Не в начале пути. Провал блоков почти не ставит — он вырезает
    // их, — поэтому с краю он не виден до последнего шага, а дно у
    // него на двадцати блоках над коренной породой. Первая же
    // пробежка от точки появления не должна кончаться падением.
    if (structs::isHomeCell(superX, superZ)) return site;

    const i32 wy = terrain->surfaceHeight(cx, cz);
    // На суше и повыше моря: провал в океане — это просто океан.
    if (wy < TerrainGenerator::SEA_LEVEL + 8) return site;

    site.exists = true;
    site.center = { cx, wy, cz };
    site.radius = r;
    // Дно на двадцати блоках над коренной породой: глубже — и падение
    // убивает всегда, мельче — и это яма, а не обрыв.
    site.bottom = 20;
    return site;
}

/// Провалы своего чанка: колонка за колонкой, как и замок.
static void applySinkholes(Chunk& c, const FeatureContext& ctx) {
    const i32 bx0 = c.coord.x * CHUNK_SIZE;
    const i32 bz0 = c.coord.z * CHUNK_SIZE;
    const i32 sc0x = (i32)std::floor((f32)bx0 / (f32)structs::SUPER_BLOCKS);
    const i32 sc0z = (i32)std::floor((f32)bz0 / (f32)structs::SUPER_BLOCKS);

    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            i32 px = 0, pz = 0, pr = 0;
            if (!sinkholeCandidate(sc0x + dx, sc0z + dz, ctx.seed, px, pz, pr))
                continue;
            // Задевает ли провал этот чанк? Венец шире устья на блок.
            const i32 reach = pr + 1;
            if (bx0 + CHUNK_SIZE - 1 < px - reach || bx0 > px + reach) continue;
            if (bz0 + CHUNK_SIZE - 1 < pz - reach || bz0 > pz + reach) continue;

            const SinkholeSite s = sinkholeAt(sc0x + dx, sc0z + dz, ctx.seed,
                                              ctx.terrain);
            if (!s.exists) continue;

            for (i32 lz = 0; lz < CHUNK_SIZE; ++lz)
                for (i32 lx = 0; lx < CHUNK_SIZE; ++lx) {
                    const i32 wx = bx0 + lx, wz = bz0 + lz;
                    const i32 ddx = wx - s.center.x, ddz = wz - s.center.z;
                    const i32 d2 = ddx * ddx + ddz * ddz;
                    const i32 rr = s.radius * s.radius;
                    if (d2 > (s.radius + 1) * (s.radius + 1)) continue;

                    // Высоту берём из УЖЕ ПОСЧИТАННЫХ колонок чанка,
                    // а не спрашиваем генератор заново: своя колонка
                    // лежит рядом и стоит ноль, а запрос к рельефу —
                    // семьсот наносекунд, и на три сотни колонок
                    // устья это четверть миллисекунды на чанк.
                    const i32 top = ctx.columnAt(lx, lz, wx, wz).surface;
                    if (d2 <= rr) {
                        // Устье: вниз до дна — пусто.
                        for (i32 y = s.bottom; y <= top + 2; ++y)
                            putWorld(c, wx, y, wz, AIR, true);
                        // Дно каменное: провалиться сквозь него в
                        // пещеру — уже не обрыв, а дыра в мире.
                        putWorld(c, wx, s.bottom - 1, wz, STONE, true);
                    } else {
                        // Венец.
                        putWorld(c, wx, top - 1, wz, STONE, true);
                    }
                }
        }
}

// ============================================================
// Замки Чёрного леса
// ============================================================
// ============================================================
//
// Огромная постройка, а не домик: стена в семьдесят блоков по
// стороне, четыре угловые башни и донжон посередине.
//
// Строится ПО КОЛОНКАМ своего чанка, а не обходом всего замка. Обход
// всего — это сто тысяч записей на каждый из чанков, которые замок
// задевает, и почти все мимо: putWorld их отбросит, но перебрать
// успеет. Колонка своего чанка даёт ровно 32x32 и ни одной лишней.
namespace castle {

constexpr i32 HALF      = CASTLE_HALF;  ///< внешний край кольца стен
constexpr i32 WALL_H    = 9;    ///< высота стены над двором
constexpr i32 TOWER_R   = 5;    ///< полбашни (11x11)
constexpr i32 TOWER_OFF = HALF - 4;  ///< где стоят башни от середины
constexpr i32 TOWER_H   = 17;
constexpr i32 KEEP_HALF = 9;    ///< полдонжона (19x19)
constexpr i32 KEEP_H    = 22;
constexpr i32 FLOOR_STEP = 6;   ///< этаж донжона через столько блоков
constexpr i32 GATE_HALF = 2;    ///< полширины ворот
constexpr i32 GATE_H    = 6;

inline i32 cheb(i32 a, i32 b) { return std::max(std::abs(a), std::abs(b)); }

/// Одна колонка замка. dx, dz — смещение от середины донжона.
void column(Chunk& c, i32 wx, i32 wz, i32 dx, i32 dz, i32 base) {
    const i32 m = cheb(dx, dz);
    if (m > HALF) return;

    // ---- Площадка ----
    //
    // Замок стоит на ровном, иначе стена уходит в склон и ворота
    // оказываются под землёй. Три блока основания вниз и чистое небо
    // вверх на всю высоту донжона.
    for (i32 y = base - 3; y < base; ++y) putWorld(c, wx, y, wz, STONE, true);
    for (i32 y = base; y <= base + KEEP_H + 3; ++y) putWorld(c, wx, y, wz, AIR, true);

    // ---- Донжон ----
    if (m <= KEEP_HALF) {
        const bool wall = (cheb(dx, dz) == KEEP_HALF);
        if (wall) {
            for (i32 y = base; y <= base + KEEP_H; ++y)
                putWorld(c, wx, y, wz, STONE, true);
            // Бойницы: каждая четвёртая колонка, начиная со второго
            // этажа. Через одну — это уже не бойницы, а колоннада:
            // половина стены исчезает.
            if (((wx + wz) & 3) == 0) {
                for (i32 f = FLOOR_STEP; f < KEEP_H; f += FLOOR_STEP)
                    putWorld(c, wx, base + f + 2, wz, AIR, true);
            }
        } else {
            // Перекрытия. Лестницы внутри нет намеренно: донжон
            // пустой, и подниматься в нём пока некуда — пол на
            // каждом этаже держит крышу и делит объём.
            for (i32 f = FLOOR_STEP; f < KEEP_H; f += FLOOR_STEP)
                putWorld(c, wx, base + f, wz, PLANK, true);
            putWorld(c, wx, base + KEEP_H, wz, STONE, true);
            // Фонарь в каждом углу первого этажа: внутри донжона
            // иначе не видно ничего.
            if (std::abs(dx) == KEEP_HALF - 1 && std::abs(dz) == KEEP_HALF - 1)
                putWorld(c, wx, base + 1, wz, LANTERN, true);
        }
        return;
    }

    // ---- Башни ----
    const i32 tdx = std::abs(dx) - TOWER_OFF;
    const i32 tdz = std::abs(dz) - TOWER_OFF;
    if (std::abs(tdx) <= TOWER_R && std::abs(tdz) <= TOWER_R) {
        const bool shell = (cheb(tdx, tdz) == TOWER_R);
        if (shell) {
            for (i32 y = base; y <= base + TOWER_H; ++y)
                putWorld(c, wx, y, wz, STONE, true);
            if (((wx + wz) & 1) == 0)
                putWorld(c, wx, base + TOWER_H + 1, wz, STONE, true);
        } else {
            putWorld(c, wx, base + TOWER_H, wz, STONE, true);
            if (tdx == 0 && tdz == 0)
                putWorld(c, wx, base + 1, wz, LANTERN, true);
        }
        return;
    }

    // ---- Стена ----
    if (m >= HALF - 1) {
        for (i32 y = base; y <= base + WALL_H; ++y)
            putWorld(c, wx, y, wz, STONE, true);
        // Зубцы: через один. Стена без них читается как забор.
        if (((wx + wz) & 1) == 0)
            putWorld(c, wx, base + WALL_H + 1, wz, STONE, true);
        return;
    }

    // ---- Двор ----
    putWorld(c, wx, base - 1, wz, STONE, true);
}

/// Ворота прорезаются ПОСЛЕ стены.
///
/// Тот же урок, что и с лестницей в дереве: проход, прорезанный до
/// стены, стена и закладывает обратно.
void gate(Chunk& c, i32 wx, i32 wz, i32 dx, i32 dz, i32 base) {
    if (std::abs(dx) > GATE_HALF) return;
    if (dz < HALF - 1 || dz > HALF) return;
    for (i32 y = base; y < base + GATE_H; ++y)
        putWorld(c, wx, y, wz, AIR, true);
}

/// Дверь донжона — тоже после его стен.
void keepDoor(Chunk& c, i32 wx, i32 wz, i32 dx, i32 dz, i32 base) {
    if (std::abs(dx) > 1 || dz != KEEP_HALF) return;
    for (i32 y = base; y < base + 3; ++y)
        putWorld(c, wx, y, wz, AIR, true);
}

} // namespace castle

/// Где БЫЛ БЫ замок этой ячейки — по одним только хэшам.
///
/// Вынесено отдельно, потому что цена у двух половин разная на два
/// порядка: хэш стоит наносекунды, а «какой тут биом» — полный
/// пересчёт шума. Лес спрашивает про замок для каждого дерева и
/// каждого куста, девять ячеек на каждое: без дешёвой половины это
/// полторы тысячи запросов биома на чанк.
static bool castleCandidate(i32 superX, i32 superZ, u64 worldSeed,
                            i32& outX, i32& outZ)
{
    // Только пустые ячейки: рядом с деревней замок читался бы как её
    // часть, а он ничей.
    if (structs::layoutFor(superX, superZ, worldSeed).kind != structs::None)
        return false;

    // Свой хэш — не тот, по которому решается логово: иначе замок и
    // логово никогда бы не встретились в одной ячейке, а должны бы.
    const u32 h = hashXZ(superX, superZ, worldSeed ^ 0xCA57);
    if ((h & 0xFF) >= 96) return false;

    // Середина ячейки с небольшим сдвигом: замок шириной в семьдесят
    // блоков, к краю ячейки его прижимать нельзя — он полезет в
    // соседнюю, где может стоять деревня.
    outX = superX * structs::SUPER_BLOCKS + 128 + (i32)((h >> 8)  & 0x1F) - 16;
    outZ = superZ * structs::SUPER_BLOCKS + 128 + (i32)((h >> 14) & 0x1F) - 16;
    return true;
}

CastleSite castleAt(i32 superX, i32 superZ, u64 worldSeed,
                    const TerrainGenerator* terrain)
{
    CastleSite site;
    if (!terrain) return site;

    i32 cx = 0, cz = 0;
    if (!castleCandidate(superX, superZ, worldSeed, cx, cz)) return site;

    // Замок стоит в Чёрном лесу, и только в нём.
    if (terrain->biomeAt(cx, cz) != Blight) return site;

    const i32 wy = terrain->surfaceHeight(cx, cz);
    if (wy < TerrainGenerator::SEA_LEVEL + 3) return site;

    site.exists = true;
    site.center = { cx, wy, cz };
    return site;
}

// ============================================================
// Логова: области, где водится один-единственный вид
// ============================================================

LairSite lairAt(i32 superX, i32 superZ, u64 worldSeed,
                const TerrainGenerator* terrain)
{
    LairSite lair;
    if (!terrain) return lair;

    // Только пустые ячейки сетки. Логово на деревне означало бы, что
    // деревня перестала быть местом, куда возвращаются.
    if (structs::layoutFor(superX, superZ, worldSeed).kind != structs::None)
        return lair;

    // И не в начале пути. Логово блоков не ставит, поэтому перебор
    // точки появления видит вокруг чистое поле — а в поле стая
    // волков, и первое, что узнаёт игрок о мире, это экран смерти.
    if (structs::isHomeCell(superX, superZ)) return lair;

    // Свой хэш, не структурный: иначе «пусто» и «логово» решались бы
    // одним числом, и всякая пустая ячейка стала бы логовом.
    const u32 h = hashXZ(superX, superZ, worldSeed ^ 0x1A12);

    // Логово — в каждой третьей пустой ячейке. Чаще — и мир станет
    // сплошным логовом; реже — игрок не встретит ни одного.
    if ((h & 0xFF) >= 85) return lair;

    const i32 baseX = superX * structs::SUPER_BLOCKS;
    const i32 baseZ = superZ * structs::SUPER_BLOCKS;
    const i32 cx = baseX + 64 + (i32)((h >> 8)  & 0x7F);   // 64..191
    const i32 cz = baseZ + 64 + (i32)((h >> 15) & 0x7F);

    // Радиус 40..71. Меньше — логово проскакивается на бегу и не
    // читается как место; больше — оно накрывает соседнюю ячейку, и
    // два логова начинают спорить за одну точку.
    const f32 radius = 40.f + (f32)((h >> 22) & 0x1F);

    const i32 wy = terrain->surfaceHeight(cx, cz);
    if (wy < TerrainGenerator::SEA_LEVEL + 2) return lair;

    lair.exists = true;
    lair.center = { cx, wy, cz };
    lair.radius = radius;
    switch ((h >> 27) & 0x3) {
        case 0:  lair.kind = LairKind::Wolves;    break;
        case 1:  lair.kind = LairKind::Skeletons; break;
        case 2:  lair.kind = LairKind::Goblins;   break;
        default: lair.kind = LairKind::Slimes;    break;
    }
    return lair;
}

LairSite lairCovering(i32 wx, i32 wz, u64 worldSeed,
                      const TerrainGenerator* terrain)
{
    const i32 sc0x = (i32)std::floor((f32)wx / (f32)structs::SUPER_BLOCKS);
    const i32 sc0z = (i32)std::floor((f32)wz / (f32)structs::SUPER_BLOCKS);

    for (i32 dz = -1; dz <= 1; ++dz)
        for (i32 dx = -1; dx <= 1; ++dx) {
            const LairSite l = lairAt(sc0x + dx, sc0z + dz, worldSeed, terrain);
            if (!l.exists) continue;
            const f32 ddx = (f32)(wx - l.center.x);
            const f32 ddz = (f32)(wz - l.center.z);
            if (ddx * ddx + ddz * ddz <= l.radius * l.radius) return l;
        }
    return LairSite{};
}

bool structureCovers(i32 wx, i32 wz, u64 worldSeed,
                     const TerrainGenerator* terrain)
{
    if (structureBlocks(wx, wz, worldSeed)) return true;
    return castleBlocks(wx, wz, worldSeed, terrain);
}

// ============================================================
// Дороги между деревнями
// ============================================================
//
// Соединяются только СОСЕДНИЕ по сетке super-chunk деревни и только в
// сторону +X и +Z. Иначе каждый отрезок строился бы дважды — с обоих
// концов, — а на перекрёстках дорог оказалось бы вдвое больше, чем
// нужно, чтобы куда-то дойти.
//
// Дорога кладётся на поверхность тем же putOnGround, что и деревенские
// дорожки: он сам находит землю и сам отказывается класть камень на
// камень или в воду. Поэтому дорога честно обрывается у реки и на
// скалах — мост через ущелье здесь не строится.
namespace {

void stampRoad(Chunk& c, const FeatureContext& ctx,
               const glm::ivec3& a, const glm::ivec3& b,
               i32 bx0, i32 bx1, i32 bz0, i32 bz1)
{
    const i32 dx = b.x - a.x, dz = b.z - a.z;
    const i32 steps = std::max(std::abs(dx), std::abs(dz));
    if (steps <= 0) return;

    // Поперёк направления: вдоль X ширина идёт по Z, и наоборот.
    const bool alongX = std::abs(dx) >= std::abs(dz);

    for (i32 i = 0; i <= steps; ++i) {
        const i32 x = a.x + dx * i / steps;
        const i32 z = a.z + dz * i / steps;
        // Шаги вне своего чанка пропускаем сразу: отрезок бывает
        // длиной в полтысячи блоков, а класть из него нужно
        // тридцать два.
        if (x < bx0 || x > bx1 || z < bz0 || z > bz1) continue;
        for (i32 w = -ROAD_HALF; w <= ROAD_HALF; ++w)
            putOnGround(c, ctx, alongX ? x : x + w,
                                alongX ? z + w : z, STONE);
    }
}

} // namespace

void applyRoads(Chunk& chunk, const FeatureContext& ctx) {
    const i32 bx0 = chunk.coord.x * CHUNK_SIZE - 2;
    const i32 bx1 = bx0 + CHUNK_SIZE + 3;
    const i32 bz0 = chunk.coord.z * CHUNK_SIZE - 2;
    const i32 bz1 = bz0 + CHUNK_SIZE + 3;

    const i32 sc0x = (i32)std::floor((f32)chunk.coord.x / (f32)structs::SUPER_CHUNKS);
    const i32 sc0z = (i32)std::floor((f32)chunk.coord.z / (f32)structs::SUPER_CHUNKS);

    // Два супер-чанка в каждую сторону: отрезок между соседями длиной
    // до двухсот пятидесяти шести блоков может пересекать этот чанк,
    // начинаясь и заканчиваясь далеко от него.
    for (i32 dz = -2; dz <= 2; ++dz)
        for (i32 dx = -2; dx <= 2; ++dx) {
            const i32 sx = sc0x + dx, sz = sc0z + dz;
            const VillageSite a = villageAt(sx, sz, ctx.seed, ctx.terrain);
            if (!a.exists) continue;

            const VillageSite east  = villageAt(sx + 1, sz, ctx.seed, ctx.terrain);
            if (east.exists)
                stampRoad(chunk, ctx, a.center, east.center, bx0, bx1, bz0, bz1);

            const VillageSite south = villageAt(sx, sz + 1, ctx.seed, ctx.terrain);
            if (south.exists)
                stampRoad(chunk, ctx, a.center, south.center, bx0, bx1, bz0, bz1);
        }
}

/// Замок в своём чанке: 32x32 колонки, ни одной лишней.
static void stampCastle(Chunk& c, const FeatureContext& ctx,
                        const CastleSite& site)
{
    const i32 bx0 = c.coord.x * CHUNK_SIZE;
    const i32 bz0 = c.coord.z * CHUNK_SIZE;
    // Чанк вне следа замка — выходим, не перебирая колонки.
    if (bx0 + CHUNK_SIZE - 1 < site.center.x - castle::HALF ||
        bx0 > site.center.x + castle::HALF ||
        bz0 + CHUNK_SIZE - 1 < site.center.z - castle::HALF ||
        bz0 > site.center.z + castle::HALF) return;

    const i32 base = site.center.y;
    (void)ctx;

    for (i32 lz = 0; lz < CHUNK_SIZE; ++lz)
        for (i32 lx = 0; lx < CHUNK_SIZE; ++lx) {
            const i32 wx = bx0 + lx, wz = bz0 + lz;
            castle::column(c, wx, wz, wx - site.center.x, wz - site.center.z,
                           base);
        }

    // Проёмы — последними, по той же причине, что и лестница в
    // дереве: прорезанное до стены стена закладывает обратно.
    for (i32 lz = 0; lz < CHUNK_SIZE; ++lz)
        for (i32 lx = 0; lx < CHUNK_SIZE; ++lx) {
            const i32 wx = bx0 + lx, wz = bz0 + lz;
            const i32 dx = wx - site.center.x, dz = wz - site.center.z;
            castle::gate(c, wx, wz, dx, dz, base);
            castle::keepDoor(c, wx, wz, dx, dz, base);
        }
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

            // Замок живёт в ПУСТОЙ ячейке, поэтому спрашивается до
            // проверки на None: в сетке структур его нет, он решается
            // своим хэшем и биомом.
            if (L.kind == structs::None) {
                const CastleSite cs = castleAt(sx, sz, ctx.seed, ctx.terrain);
                if (cs.exists) stampCastle(chunk, ctx, cs);
                continue;
            }

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
                case structs::TreeDungeon:
                    stampTreeDungeon(chunk, ctx, L); break;
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
                } else if (col.lavaTop > 0 && y <= col.lavaTop) {
                    // Кратер вулкана. Наливается здесь, а не в
                    // applyLiquids: та знает только про уровень моря,
                    // а жерло стоит много выше него.
                    id = LAVA;
                }

                // Снеговая линия: выше неё вершина под снегом, и
                // видно это за половину карты.
                if (id == biome.surfaceBlock && y == surface - 1 &&
                    surface > TerrainGenerator::SNOW_LINE &&
                    col.climate.biome == Mountains) {
                    id = SNOW;
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
    applyRivers(chunk, fctx);
    applyStructures(chunk, fctx);
    // Дороги ПОСЛЕ построек: они идут от деревни к деревне, и
    // деревенская мостовая должна остаться сверху, а не под ними.
    applyRoads(chunk, fctx);
    applyTrees(chunk, fctx);
    // Тайник — под землёй, до провалов: провал, попавший на руины,
    // вскроет камеру сверху, и это честно.
    // Бочаги — до тайников и провалов: те выгрызают землю, и
    // вода, налитая после них, повисла бы над ямой.
    applySwampPools(chunk, fctx);
    applyTreasures(chunk, fctx);
    // Провал — последним: он выгрызает всё, что над ним поставили.
    // Дерево, выросшее посреди устья, повисло бы в воздухе.
    applySinkholes(chunk, fctx);
}

} // namespace world

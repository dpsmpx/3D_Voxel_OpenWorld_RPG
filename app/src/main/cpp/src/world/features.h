#pragma once
#include "chunk.h"
#include "terrain.h"
#include "../core/types.h"
#include <glm/glm.hpp>
#include <vector>

namespace world {

// ============================================================
// Контекст генерации — то, что доступно любой фиче.
// Содержит ссылку на terrain и seed мира.
// ============================================================
struct FeatureContext {
    const TerrainGenerator* terrain;
    u64 seed;
};

// ============================================================
// Features применяются к чанку ПОСЛЕ terrain-слоёв, но ДО
// построения меша. Каждая фича пишет в voxels[] по локальным
// координатам.
//
// Система рассчитана на детерминированность: один и тот же
// (seed, chunk) всегда даёт один результат.
// ============================================================

// --- Деревья: анкерные позиции в чанке, canopy может пересекать границу ---
void applyTrees(Chunk& chunk, const FeatureContext& ctx);

// --- Пещеры: карвинг после слоёв, до руд ---
void applyCaves(Chunk& chunk, const FeatureContext& ctx);

// --- Руды: жилы после карвинга ---
void applyOres(Chunk& chunk, const FeatureContext& ctx);

// --- Вода/лава: заполнение уровней ---
void applyLiquids(Chunk& chunk, const FeatureContext& ctx);

// --- Структуры: деревни, подземелья, руины, алтари ---
// Работают через super-chunk grid: 8×8 чанков = 256×256 блоков.
// Структура ставится детерминированно по (superX, superZ).
// При генерации чанка проверяются все super-чанки в радиусе 2 —
// структура может пересекать границы.
void applyStructures(Chunk& chunk, const FeatureContext& ctx);

// ============================================================
// Утилиты, доступные features
// ============================================================
namespace feat_util {

// Записать блок в локальные координаты, если внутри чанка.
inline void put(Chunk& c, i32 lx, i32 ly, i32 lz, u16 block, bool overwrite = true) {
    if (!c.inBounds(lx, ly, lz)) return;
    if (!overwrite && c.at(lx, ly, lz) != AIR) return;
    c.voxels[chunkIndex(lx, ly, lz)] = block;
}

// Записать блок в мировых координатах — может попасть в соседний чанк.
// В нашей системе это НЕ применяется (features работают только на
// локальных координатах своего чанка, а структуры на границах
// дублируются при генерации каждого из соседей через grid-hash).
inline void putWorld(Chunk& c, i32 wx, i32 wy, i32 wz, u16 block, bool overwrite = true) {
    i32 lx = wx - c.coord.x * CHUNK_SIZE;
    i32 lz = wz - c.coord.z * CHUNK_SIZE;
    put(c, lx, wy, lz, block, overwrite);
}

// Детерминированный хэш (x, z, seed)
inline u32 hashXZ(i32 x, i32 z, u64 seed) {
    u64 h = (u64)(u32)x * 0x9E3779B97F4A7C15ULL;
    h ^= (u64)(u32)z * 0xC4CEB9FE1A85EC53ULL;
    h ^= seed;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return (u32)h;
}

// Детерминированный хэш (x, y, z, seed)
inline u32 hashXYZ(i32 x, i32 y, i32 z, u64 seed) {
    u64 h = (u64)(u32)x * 0x9E3779B97F4A7C15ULL;
    h ^= (u64)(u32)y * 0xC4CEB9FE1A85EC53ULL;
    h ^= (u64)(u32)z * 0xFF51AFD7ED558CCDULL;
    h ^= seed;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return (u32)h;
}

} // namespace feat_util

} // namespace world
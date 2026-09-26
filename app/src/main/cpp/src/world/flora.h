/**
 * @file flora.h
 * @brief Мир: где что растёт — растения и мелкая природная мелочь.
 *
 * Растение появляется не потому, что жребий велел «поставить дерево»,
 * а потому, что место к этому располагает. Решение складывается по
 * цепочке, и каждое звено берёт данные предыдущих:
 *
 *   биом (климат, высота)            → что здесь вообще растёт
 *   рощи и поляны (медленное поле)   → где лес гуще, где прогалина
 *   вода (близость реки, берег)      → у воды гуще; тростник у кромки
 *   рельеф (крутизна, высота)        → на отвесе и выше границы леса — нет
 *   земля (что лежит сверху, воздух) → на дороге и в доме — нет
 *
 * Места выбираются по сетке мировых клеток своего размера для каждого
 * класса (дерево — 4 блока, куст — 3, трава — 2): одна кандидатура на
 * клетку со сдвигом по хэшу. Это дешёвое подобие «синего шума»: соседи
 * не слипаются, а плотность управляется вероятностью. Сетка мировая,
 * поэтому решение не зависит от того, в каком порядке грузятся чанки.
 */
#pragma once
#include "features.h"
#include "flora_types.h"
#include "../core/types.h"

namespace world {

/// Что растёт в биоме. Плотности — доли кандидатур, которые
/// принимаются при полной «рощевости» места.
struct FloraProfile {
    f32       cover = 0.f;          ///< деревья: доля принятых клеток 4x4
    FloraKind tree = FloraKind::Oak;
    FloraKind tree2 = FloraKind::Oak;
    f32       mix2 = 0.f;           ///< доля второго вида
    f32       patchiness = 0.5f;    ///< 0 — лес ровный, 1 — рощи и поляны
    f32       understory = 0.f;     ///< кусты/папоротник: клетки 3x3
    FloraKind shrub = FloraKind::Bush;
    f32       grass = 0.f;          ///< пучки травы: клетки 2x2
    f32       flowers = 0.f;        ///< цветочные поляны
    f32       rocks = 0.f;          ///< камни и галька
    f32       deadfall = 0.f;       ///< коряги, пни, грибы в лесу
    f32       reeds = 0.f;          ///< тростник у воды
    f32       treeScale = 1.f;      ///< тундра и высокогорье — деревья ниже
};

const FloraProfile& floraProfile(BiomeId b);

/// Рощевость места 0..1: где лес собран в массив, где прогалина.
/// Медленное поле — рощи в сотню-другую блоков, поляны между ними.
f32 floraGrove(const TerrainGenerator& terrain, i32 wx, i32 wz);

/// Доля деревьев в точке, 0..1 — то, что кладёт генерация, без
/// проверки земли и построек. Нужна отладочной карте и проверкам.
f32 floraTreeCover(const TerrainGenerator& terrain, const TerrainGenerator::Column& col,
                   i32 wx, i32 wz, f32 near);

/// Густота мелочи в точке, 0..1: трава, цветы, подлесок, камни — по
/// тем же вероятностям, что у генерации, без проверки земли. Для
/// отладочной карты.
f32 floraDecorDensity(const TerrainGenerator& terrain, const TerrainGenerator::Column& col,
                      i32 wx, i32 wz, f32 near);

/// Растения чанка: заполняет chunk.flora и ставит невидимые стволы
/// деревьев. Зовётся из generateChunkVoxels после построек и дорог —
/// растения садятся на готовую землю и обходят построенное.
void placeFlora(Chunk& chunk, const FeatureContext& ctx);

/// Убирает растения, которым поздние шаги генерации (провалы) выбили
/// землю, — вместе со стволом: дерево не висит над ямой.
void pruneFlora(Chunk& chunk);

/// Видно ли растение при текущих вокселях: цела ли земля под ним, не
/// занято ли место, стоит ли ствол. voxel(x, y, z) — блок в локальных
/// координатах чанка (y — мировая высота).
template <class VoxelFn>
bool floraStillStands(const FloraInstance& f, VoxelFn&& voxel);

// ---- реализация шаблона ----

bool floraGroundOk(FloraKind k, u16 ground);

template <class VoxelFn>
bool floraStillStands(const FloraInstance& f, VoxelFn&& voxel) {
    const i32 y = f.y;
    if (y < 1 || y >= CHUNK_SIZE_Y - 1) return false;
    if (floraIsTree(f.kind)) {
        // Дерево стоит, пока цел его ствол: срубленное уходит целиком
        // (ChunkManager::setVoxel), но и огонь, и лопата могут
        // тронуть один блок.
        const u16 core = f.kind == FloraKind::Cactus ? CACTUS_CORE : TRUNK;
        if (f.trunk == 0) return false;
        for (i32 k = 0; k < f.trunk; ++k)
            if (voxel(f.lx, y + k, f.lz) != core) return false;
        return true;
    }
    if (!floraGroundOk(f.kind, voxel(f.lx, y - 1, f.lz))) return false;
    return voxel(f.lx, y, f.lz) == AIR;
}

} // namespace world

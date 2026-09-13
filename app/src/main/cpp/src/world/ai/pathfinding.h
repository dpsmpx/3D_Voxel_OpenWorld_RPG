/**
 * @file pathfinding.h
 * @brief Навигация по воксельной сетке: поиск пути A*.
 */
#pragma once
#include "../../core/types.h"
#include "../chunk_manager.h"
#include <glm/glm.hpp>
#include <vector>

namespace world::ai {

/// Проходимость вокселя для моба:
///   - текущий и верхний блок должны быть не-solid (не мешать телу)
///   - блок под ногами ДОЛЖЕН быть solid (иначе моб "падает")
/// Исключения: allowFall=true — можно падать вниз на любую высоту.
struct MoveParams {
    f32 bodyHeight   = 1.5f;
    bool allowFall   = true;
    bool allowWater  = true;
    bool allowJump   = true;    // прыжок через 1 блок
    i32 maxFallDepth = 24;
};

/// Проверка: может ли моб стоять в позиции (x, y, z) — координаты
/// блока, в котором находятся ступни.
///
/// Читает мир через курсор, а не через менеджер: проверки ходят по
/// соседним клеткам, то есть почти всегда по одному и тому же чанку,
/// и поиск чанка на каждый воксель здесь — основная цена.
bool isStandable(world::VoxelReader& rd,
                 i32 x, i32 y, i32 z, const MoveParams& mp);

/// Может ли моб перейти из (x,y,z) в (nx,ny,nz) — одна клетка.
/// Проверяет все необходимые условия и возвращает стоимость шага.
bool canStep(world::VoxelReader& rd,
             i32 x, i32 y, i32 z,
             i32 nx, i32 ny, i32 nz,
             const MoveParams& mp,
             f32& stepCost);

/// A* с 4-связной топологией + прыжки/падения.
/// Ограничение итераций — защита от фризов на больших дистанциях.
struct PathResult {
    bool ok = false;
    std::vector<glm::ivec3> waypoints;   // последовательность блоков (y-ступни)
    f32 length = 0.f;
};

PathResult findPath(world::ChunkManager& world,
                    const glm::ivec3& startBlock,
                    const glm::ivec3& goalBlock,
                    const MoveParams& mp,
                    i32 maxIterations = 2000);

/// Упрощение пути: удаляем промежуточные точки, где есть прямая видимость.
void smoothPath(world::ChunkManager& world,
                std::vector<glm::ivec3>& path,
                const MoveParams& mp);

} // namespace world::ai

/**
 * @file raycast.h
 * @brief Физика: AABB-коллизия с вокселями, raycast, контроллер персонажа.
 */
#pragma once
#include "../core/types.h"
#include "../world/chunk_manager.h"
#include <glm/glm.hpp>

namespace physics {

struct RayHit {
    bool       hit      = false;
    glm::ivec3 block    {0};
    glm::ivec3 normal   {0};   // единичный вектор, указывает на грань входа (против луча)
    f32        distance = 0.f;
    u16        blockType = 0;
};

/// DDA-обход воксельной сетки (Amanatides & Woo, 1987).
/// Возвращает первое попадание в твёрдый блок или miss.
RayHit raycastVoxels(world::ChunkManager& world,
                     const glm::vec3& origin,
                     const glm::vec3& dir,
                     f32 maxDist);

} // namespace physics

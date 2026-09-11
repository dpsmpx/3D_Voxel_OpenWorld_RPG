/**
 * @file collision.h
 * @brief Физика: AABB-коллизия с вокселями, raycast, контроллер персонажа.
 */
#pragma once
#include "../core/types.h"
#include "../world/chunk_manager.h"
#include <glm/glm.hpp>

namespace physics {

struct CollisionFlags {
    bool hitX = false;
    bool hitY = false;
    bool hitZ = false;
    bool onGround  = false;
    bool onCeiling = false;
};

/// AABB игрока: pos — точка на ступнях по центру основания.
/// halfWidth — половина по X и Z. height — полная высота.
struct PlayerBox {
    f32 halfWidth = 0.30f;
    f32 height    = 1.80f;

    glm::vec3 min(const glm::vec3& pos) const {
        return { pos.x - halfWidth, pos.y, pos.z - halfWidth };
    }
    glm::vec3 max(const glm::vec3& pos) const {
        return { pos.x + halfWidth, pos.y + height, pos.z + halfWidth };
    }
    glm::vec3 center(const glm::vec3& pos) const {
        return { pos.x, pos.y + height * 0.5f, pos.z };
    }
    glm::vec3 eye(const glm::vec3& pos, f32 eyeHeight = 1.62f) const {
        return { pos.x, pos.y + eyeHeight, pos.z };
    }
};

/// Проверка пересечения AABB с твёрдыми вокселями.
bool overlapsSolid(world::ChunkManager& world,
                   const glm::vec3& bmin,
                   const glm::vec3& bmax);

/// Разрешает движение по осям (Y → X → Z). Модифицирует pos и delta.
/// Возвращает флаги столкновений.
CollisionFlags resolveMovement(world::ChunkManager& world,
                               glm::vec3& pos,
                               glm::vec3& delta,
                               const PlayerBox& box);

} // namespace physics

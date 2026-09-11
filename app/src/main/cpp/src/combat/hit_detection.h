/**
 * @file hit_detection.h
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "damage.h"
#include "spatial_hash.h"
#include <glm/glm.hpp>
#include <vector>

namespace combat {

struct HitTarget {
    ecs::Entity entity{};
    glm::vec3   center{ 0.f };
    f32         distance = 0.f;
};

bool losClear(world::ChunkManager& world,
              const glm::vec3& from,
              const glm::vec3& to);

/// Melee-конус. Использует SpatialHash для быстрого поиска.
/// Передай nullptr в hash, если хочешь fallback на полный перебор.
void meleeConeHits(world::ChunkManager& world,
                   ecs::Registry& reg,
                   const SpatialHash* hash,
                   const glm::vec3& origin,
                   const glm::vec3& forward,
                   f32 reach,
                   f32 coneAngle,
                   u32 attackerFaction,
                   ecs::Entity excludeEntity,
                   std::vector<HitTarget>& out);

void sphereHits(world::ChunkManager& world,
                ecs::Registry& reg,
                const SpatialHash* hash,
                const glm::vec3& center,
                f32 radius,
                u32 attackerFaction,
                ecs::Entity excludeEntity,
                std::vector<HitTarget>& out);

void queryNearby(ecs::Registry& reg,
                 const glm::vec3& center,
                 f32 maxDistance,
                 std::vector<HitTarget>& out);

} // namespace combat

/**
 * @file combat_controller.h
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "../physics/raycast.h"
#include "components.h"
#include "weapon.h"
#include "hit_detection.h"
#include "spatial_hash.h"
#include <glm/glm.hpp>
#include <vector>

namespace combat {

struct CombatInput {
    bool attackPressed = false;
    bool attackHeld    = false;
    bool finisherInput = false;
};

struct CombatAction {
    bool      didMeleeHit    = false;
    bool      didCastSpell   = false;
    bool      didShoot       = false;
    bool      didFinisher    = false;
    i32       hitCount       = 0;
    f32       totalDamage    = 0.f;
    glm::vec3 hitPoint{0};
};

/// Phase 15: добавлен параметр spatial hash (может быть nullptr).
void updateCombat(world::ChunkManager& world,
                  ecs::Registry& reg,
                  const SpatialHash* hash,
                  ecs::Entity entity,
                  const glm::vec3& origin,
                  const glm::vec3& aimDir,
                  const CombatInput& in,
                  f32 dt,
                  CombatAction& out);

bool tryStartAttack(ecs::Registry& reg,
                    ecs::Entity entity);

} // namespace combat

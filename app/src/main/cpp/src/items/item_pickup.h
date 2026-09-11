/**
 * @file item_pickup.h
 * @brief Предметы: определения, инвентарь, лут, подбор, использование.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "item_stack.h"
#include "inventory.h"
#include <glm/glm.hpp>

namespace items {

struct ItemPickup {
    ItemStack stack{};
    f32       lifeTime      = 300.f;
    f32       lifeRemaining = 300.f;
    f32       pickupRadius  = 1.2f;

    glm::vec3 velocity{0};
    f32       groundedTime  = 0.f;
    f32       blinkTimer    = 0.f;
    f32       pickDelay     = 0.3f;

    /// Phase 15: физика
    bool      onGround = false;
};

ecs::Entity spawnPickup(ecs::Registry& reg,
                        const glm::vec3& position,
                        const ItemStack& stack,
                        const glm::vec3& initialVelocity = glm::vec3(0));

/// Phase 15: обновление с raycast-коллизией против вокселей.
void updatePickups(world::ChunkManager& world,
                   ecs::Registry& reg,
                   ecs::Entity playerEntity,
                   const glm::vec3& playerPos,
                   f32 dt);

} // namespace items

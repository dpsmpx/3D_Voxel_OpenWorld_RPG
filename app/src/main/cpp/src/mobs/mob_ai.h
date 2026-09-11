#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "../world/ai/pathfinding.h"
#include <glm/glm.hpp>
#include <vector>

namespace mobs {

// ============================================================
// Компоненты ECS, специфичные для мобов.
// ============================================================
struct MobTag {
    u16 id = 0;   // MobId
};

struct MobAI {
    ecs::Entity target{};
    glm::vec3   homePos{0};
    f32         stateTime = 0.f;
    f32         attackCooldown = 0.f;
    f32         repathCooldown = 0.f;
    f32         wanderTimer = 0.f;
    glm::vec3   wanderTarget{0};

    // Навигация
    std::vector<glm::ivec3> path;
    i32         pathIndex = 0;
    world::ai::MoveParams moveParams{};

    // Анимация
    f32 walkPhase = 0.f;
    f32 attackAnim = 0.f;
    f32 deathTimer = 0.f;
    f32 damageFlash = 0.f;

    // Флаги
    bool inWater = false;
    bool onGround = false;

    // Phase 12: лут выпал (защита от дублей)
    bool lootDropped = false;
};

// ============================================================
// Основной апдейт мобов.
// ============================================================
void updateMobs(world::ChunkManager& world,
                ecs::Registry& reg,
                ecs::Entity playerEntity,
                const glm::vec3& playerPos,
                f32 dt);

// ============================================================
// Хелперы.
// ============================================================
void dealDamage(ecs::Registry& reg, ecs::Entity target, f32 dmg);
void onMobDeath(ecs::Registry& reg, world::ChunkManager& world, ecs::Entity mob);

} // namespace mobs

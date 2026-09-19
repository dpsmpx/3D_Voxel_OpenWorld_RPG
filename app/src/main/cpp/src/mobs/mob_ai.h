/**
 * @file mob_ai.h
 * @brief Мобы: определения, конечный автомат ИИ, спавн, боссы.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "../world/ai/pathfinding.h"
#include <glm/glm.hpp>
#include <vector>

namespace mobs {

/// Компоненты ECS, специфичные для мобов.
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

    /// Навигация
    std::vector<glm::ivec3> path;
    i32         pathIndex = 0;
    world::ai::MoveParams moveParams{};

    /// Анимация
    f32 attackAnim = 0.f;
    f32 deathTimer = 0.f;
    f32 damageFlash = 0.f;

    /// Флаги
    bool inWater = false;
    bool onGround = false;

    /// Phase 12: лут выпал (защита от дублей)
    bool lootDropped = false;

    // ---- Бой с боссом (ТЗ 4.5) ----
    /// Текущая фаза, 0 — первая. Растёт по мере падения здоровья.
    u8  bossPhase      = 0;
    /// Сколько обычных ударов прошло с последнего удара по площади.
    u8  slamCounter    = 0;
    /// Отсчёт до входа в новую фазу: босс замирает и «ревёт».
    f32 phaseRoarTimer = 0.f;

    /// Сколько до следующего самолечения. Только для тех, у кого оно
    /// есть (healPeriod > 0).
    f32 healCooldown   = 0.f;
};

/// Фаза боя с боссом по доле оставшегося здоровья.
/// Например, при phaseCount = 3: >2/3 — фаза 0, >1/3 — фаза 1,
/// ниже — фаза 2 (ярость).
inline u8 bossPhaseFor(f32 healthFraction, u8 phaseCount) {
    if (phaseCount <= 1) return 0;
    const f32 step = 1.f / (f32)phaseCount;
    for (u8 p = 0; p + 1 < phaseCount; ++p)
        if (healthFraction > 1.f - step * (f32)(p + 1)) return p;
    return (u8)(phaseCount - 1);
}

/// Основной апдейт мобов.
void updateMobs(world::ChunkManager& world,
                ecs::Registry& reg,
                ecs::Entity playerEntity,
                const glm::vec3& playerPos,
                f32 dt);

/// Хелперы.
void dealDamage(ecs::Registry& reg, ecs::Entity target, f32 dmg);

// Здесь объявлялась onMobDeath() — «выпадение лута и начисление опыта
// убийце». Тело у неё было пустое, а звать её никто не звал: и лут, и
// опыт давно считает сам updateMobs(), по флагу AIAgent::Dead. Осталась
// только подпись с описанием того, чего она не делала.

} // namespace mobs

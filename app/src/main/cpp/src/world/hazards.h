/**
 * @file hazards.h
 * @brief Мир: ловушки в подземельях и замках.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "chunk_manager.h"
#include <vector>
#include <glm/glm.hpp>

namespace hazards {

/// Ловушка — сущность ECS, а не блок.
///
/// Блока-ловушки в игре нет, и заводить его пришлось бы вместе с
/// текстурой, мешированием и физикой ради одного случая. Сущность
/// же нужна и так: у ловушки есть состояние (взведена или нет) и
/// перезарядка, а у блока состояния нет.
struct Trap {
    f32  radius   = 1.8f;   ///< на каком расстоянии срабатывает
    f32  damage   = 24.f;
    bool armed    = true;
    f32  rearm    = 0.f;    ///< сколько до повторного взвода
};

/// Постоянный ключ ловушки: по нему она не задваивается при
/// повторном входе в ту же комнату.
struct TrapTag { u64 key = 0; };

/// Где стоят ловушки рядом с точкой.
///
/// Считается из той же раскладки структур, что и сами постройки:
/// ловушка — часть замка и подземелья, а не отдельная случайность.
struct TrapPoint {
    glm::ivec3 pos{0};
    u64        key = 0;
};
void trapPointsNear(const glm::vec3& around, u64 worldSeed,
                    const world::TerrainGenerator& terrain,
                    std::vector<TrapPoint>& out);

/// Заводит и убирает ловушки вокруг игрока.
class TrapSpawner {
public:
    void update(world::ChunkManager& world, ecs::Registry& reg,
                const glm::vec3& playerPos, u64 worldSeed, f32 dt);

    u32 activeTrapCount() const { return activeCount_; }

private:
    f32 timer_ = 0.f;
    u32 activeCount_ = 0;

    static constexpr f32 SPAWN_DIST   = 64.f;
    static constexpr f32 DESPAWN_DIST = 96.f;
};

/// Срабатывание: кто наступил, тому и достаётся.
///
/// @return сколько урона нанесено за этот кадр (0 — никто не попался)
f32 tickTraps(ecs::Registry& reg, ecs::Entity victim,
              const glm::vec3& victimPos, f32 dt);

} // namespace hazards

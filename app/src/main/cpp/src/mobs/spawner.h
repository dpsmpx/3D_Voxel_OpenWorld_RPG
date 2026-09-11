/**
 * @file spawner.h
 * @brief Мобы: определения, конечный автомат ИИ, спавн, боссы.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "../world/terrain.h"
#include "../world/day_cycle.h"
#include "../world/features.h"
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>

namespace mobs {

class Spawner {
public:
    /// day — игровые сутки: от них зависит, какие мобы появятся.
    void update(world::ChunkManager& world,
                ecs::Registry& reg,
                const glm::vec3& playerPos,
                const world::DayCycle& day,
                u64 worldSeed,
                f32 dt);

    u32 mobCount() const { return mobCount_; }

private:
    /// Лимиты
    static constexpr u32 MAX_MOBS_TOTAL    = 60;
    static constexpr u32 MAX_MOBS_PER_CHUNK = 3;
    static constexpr f32 SPAWN_RADIUS       = 40.f;
    static constexpr f32 SPAWN_RADIUS_MIN   = 16.f;
    static constexpr f32 DESPAWN_RADIUS     = 64.f;

    f32 spawnTimer_ = 0.f;
    f32 despawnTimer_ = 0.f;
    f32 bossTimer_ = 0.f;
    u32 mobCount_ = 0;

    /// Подземелья, в которых босс уже поставлен: второй раз не спавним.
    std::unordered_set<u64> bossPlaced_;

    /// Track per-chunk mob count (координаты чанка → счётчик)
    std::unordered_map<world::ChunkCoord, u8, world::ChunkCoordHash> perChunk_;

    u16 pickMobId(world::TerrainGenerator& gen, i32 x, i32 z, bool night,
                  u32 rng) const;

    /// Ставит боссов в подземельях рядом с игроком. Босс появляется
    /// один раз на подземелье и не участвует в обычном спавне —
    /// требование ТЗ 4.5.
    void updateBosses(world::ChunkManager& world, ecs::Registry& reg,
                      const glm::vec3& playerPos, u64 worldSeed, f32 dt);

    /// Освещённость точки, [0, 1]: небесный свет, если над точкой
    /// открытое небо, иначе темнота пещеры. Враждебные мобы
    /// появляются только в темноте — требование ТЗ 4.5.
    static f32 lightAt(world::ChunkManager& world, const world::DayCycle& day,
                       i32 x, i32 y, i32 z);
    ecs::Entity spawnMob(world::ChunkManager& world,
                         ecs::Registry& reg,
                         u16 mobId,
                         const glm::vec3& pos);
};

} // namespace mobs

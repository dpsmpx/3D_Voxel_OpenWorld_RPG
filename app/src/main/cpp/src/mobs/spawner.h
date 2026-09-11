#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "../world/terrain.h"
#include <unordered_map>
#include <glm/glm.hpp>

namespace mobs {

class Spawner {
public:
    void update(world::ChunkManager& world,
                ecs::Registry& reg,
                const glm::vec3& playerPos,
                f32 timeSec,
                f32 dt);

    u32 mobCount() const { return mobCount_; }

private:
    // Лимиты
    static constexpr u32 MAX_MOBS_TOTAL    = 60;
    static constexpr u32 MAX_MOBS_PER_CHUNK = 3;
    static constexpr f32 SPAWN_RADIUS       = 40.f;
    static constexpr f32 SPAWN_RADIUS_MIN   = 16.f;
    static constexpr f32 DESPAWN_RADIUS     = 64.f;

    f32 spawnTimer_ = 0.f;
    f32 despawnTimer_ = 0.f;
    u32 mobCount_ = 0;

    // Track per-chunk mob count (координаты чанка → счётчик)
    std::unordered_map<world::ChunkCoord, u8, world::ChunkCoordHash> perChunk_;

    bool isNight(f32 timeSec) const;
    u16 pickMobId(world::TerrainGenerator& gen, i32 x, i32 z, bool night,
                  u32 rng) const;
    ecs::Entity spawnMob(world::ChunkManager& world,
                         ecs::Registry& reg,
                         u16 mobId,
                         const glm::vec3& pos);
};

} // namespace mobs

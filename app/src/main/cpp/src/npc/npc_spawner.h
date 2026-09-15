/**
 * @file npc_spawner.h
 * @brief NPC: роли, диалоги с ветвлением, поведение жителей.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"

namespace npc {

/// Детерминированный спавнер NPC.
///
/// Идея: деревни генерируются в world::features::applyStructures по
/// super-chunk grid (8×8 чанков = 256×256 блоков). Раскладку каждой
/// спавнер СПРАШИВАЕТ у генератора (world::villageAt) — раньше он
/// повторял её правила у себя, с комментарием «совпадает с
/// features::structs::layoutFor». Две копии одного правила расходятся
/// молча: сдвинутый порог — и жители стоят в поле, где деревни нет.
///
/// NPC не хранятся в мире — они создаются/удаляются по мере
/// приближения/удаления игрока.
class NpcSpawner {
public:
    void update(world::ChunkManager& world,
                ecs::Registry& reg,
                const glm::vec3& playerPos,
                u64 worldSeed);

    u32 activeNpcCount() const { return activeCount_; }

private:
    static constexpr i32 SUPER_CHUNKS = 8;
    static constexpr i32 SUPER_BLOCKS = SUPER_CHUNKS * 32;   // 256

    static constexpr f32 SPAWN_DIST   = 200.f;
    static constexpr f32 DESPAWN_DIST = 260.f;

    f32 spawnTimer_ = 0.f;
    f32 despawnTimer_ = 0.f;
    u32 activeCount_ = 0;
};

} // namespace npc

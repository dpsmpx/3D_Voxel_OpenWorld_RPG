#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "recipe.h"
#include <glm/glm.hpp>

namespace crafting {

// ============================================================
// Станция крафта — компонент ECS.
// Создаётся в деревнях, не двигается, живёт как объект мира.
// ============================================================
struct CraftingStation {
    StationType type = StationType::Workbench;
    f32         interactRadius = 3.0f;

    // Модель — минимальный набор коробок для рендера (Phase 13 расширит).
    // Пока всё рисуется как один блок-маркер.
    u32         colorRGBA = 0x808080FF;
    f32         sizeX = 0.9f;
    f32         sizeY = 0.9f;
    f32         sizeZ = 0.9f;
};

// ============================================================
// Детерминированный спавн станций в деревнях.
// Использует ту же логику выбора структур, что и npc_spawner,
// но размещает станции рядом с домами.
// ============================================================
class StationSpawner {
public:
    void update(ecs::Registry& reg,
                world::ChunkManager& world,
                const glm::vec3& playerPos,
                u64 worldSeed);

    u32 activeStationCount() const { return activeCount_; }

private:
    f32 spawnTimer_   = 0.f;
    f32 despawnTimer_ = 0.f;
    u32 activeCount_  = 0;

    static constexpr i32 SUPER_CHUNKS = 8;
    static constexpr i32 SUPER_BLOCKS = 256;
    static constexpr f32 SPAWN_DIST   = 180.f;
    static constexpr f32 DESPAWN_DIST = 240.f;
};

} // namespace crafting

#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "npc_def.h"

namespace npc {

// ============================================================
// Обновление всех NPC: FSM, физика, реакции на игрока и врагов.
//
// Правила:
//   - Guard: атакует врагов в радиусе; ведёт себя как стражник.
//   - QuestGiver / Trader / Blacksmith / Healer: Idle / Wander,
//     останавливаются при диалоге.
//   - Villager: Idle / Wander, убегают от врагов.
//   - Все NPC: при получении урона переходят в Flee/Combat.
// ============================================================
void updateNpcs(world::ChunkManager& world,
                ecs::Registry& reg,
                ecs::Entity playerEntity,
                const glm::vec3& playerPos,
                f32 dt);

// ============================================================
// Найти ближайшего NPC, доступного для взаимодействия.
// Возвращает 0, если рядом нет никого.
// ============================================================
ecs::Entity findInteractableNpc(ecs::Registry& reg,
                                const glm::vec3& playerPos,
                                f32 maxDist);

} // namespace npc
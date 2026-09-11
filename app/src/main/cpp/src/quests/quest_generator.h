#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "../mobs/mob_def.h"
#include "../factions/faction.h"
#include "quest_def.h"

namespace quests {

// ============================================================
// Опции генерации. Передаются от NPC-квестодателя.
// ============================================================
struct QuestGenOptions {
    u32                 giverEntity   = 0;
    u32                 ownerEntity   = 0;
    factions::FactionId faction       = factions::FactionId::Villagers;
    factions::ReputationTier repTier  = factions::ReputationTier::Neutral;

    // Позиция NPC (для генерации Explore / Defend / Collect)
    glm::ivec3          giverPos{ 0 };

    // Уровень игрока — определяет сложность
    u32                 playerLevel   = 1;

    // Допустимые типы квестов (у разных NPC разные)
    bool                allowKill     = true;
    bool                allowCollect  = true;
    bool                allowExplore  = true;
    bool                allowDefend   = false;
    bool                allowDeliver  = false;

    // seed для детерминированной генерации
    u64                 seed          = 0;
};

// ============================================================
// Сгенерировать квест. Создаёт ECS-сущность с компонентом Quest
// в состоянии Available (ещё не принят игроком).
//
// Не привязывается к журналу игрока — это делает NPC при диалоге.
// ============================================================
ecs::Entity generateQuest(ecs::Registry& reg,
                          world::ChunkManager& world,
                          const QuestGenOptions& options);

// ============================================================
// Отдельные шаги — публичные на случай ручной сборки квестов.
// ============================================================
QuestDifficulty pickDifficulty(u32 playerLevel, u64 seed);
QuestType       pickType(const QuestGenOptions& options, u64 seed);
u16             pickMobId(world::ChunkManager& world,
                          const glm::ivec3& giverPos,
                          const QuestGenOptions& options, u64 seed);
u16             pickBlockId(world::ChunkManager& world,
                            const glm::ivec3& giverPos, u64 seed);
glm::ivec3      pickExploreLocation(world::ChunkManager& world,
                                    const glm::ivec3& giverPos,
                                    QuestDifficulty diff, u64 seed);

// ============================================================
// Заполнить награды и имя/описание у свежего Quest.
// ============================================================
void finalizeQuest(Quest& q,
                   const QuestGenOptions& options,
                   world::ChunkManager& world);

// ============================================================
// Выдать награду игроку за квест. Возвращает true при успехе.
// ============================================================
bool grantRewards(ecs::Registry& reg, u32 playerEntity, const Quest& q);

} // namespace quests

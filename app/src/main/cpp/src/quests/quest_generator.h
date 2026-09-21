/**
 * @file quest_generator.h
 * @brief Квесты: шаблоны, процедурная генерация, журнал заданий.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "../mobs/mob_def.h"
#include "../factions/faction.h"
#include "quest_def.h"

namespace quests {

/// Опции генерации. Передаются от NPC-квестодателя.
struct QuestGenOptions {
    u32                 giverEntity   = 0;
    u32                 ownerEntity   = 0;
    factions::FactionId faction       = factions::FactionId::Villagers;
    factions::ReputationTier repTier  = factions::ReputationTier::Neutral;

    /// Позиция NPC (для генерации Explore / Defend / Collect)
    glm::ivec3          giverPos{ 0 };

    /// Уровень игрока — определяет сложность
    u32                 playerLevel   = 1;

    /// Допустимые типы квестов (у разных NPC разные)
    bool                allowKill     = true;
    bool                allowCollect  = true;
    bool                allowExplore  = true;
    bool                allowDefend   = false;
    bool                allowDeliver  = false;
    /// Подсказка к тайнику. Её даёт не всякий: клад помнят старики,
    /// а не первый встречный торговец.
    bool                allowTreasure = false;

    /// seed для детерминированной генерации
    u64                 seed          = 0;
};

/// Сгенерировать квест. Создаёт ECS-сущность с компонентом Quest
/// в состоянии Available (ещё не принят игроком).
///
/// Не привязывается к журналу игрока — это делает NPC при диалоге.
ecs::Entity generateQuest(ecs::Registry& reg,
                          world::ChunkManager& world,
                          const QuestGenOptions& options);

/// Отдельные шаги — публичные на случай ручной сборки квестов.
QuestDifficulty pickDifficulty(u32 playerLevel, u64 seed);
/// Выбирает тип квеста детерминированно по seed.
/// @param options какие типы разрешены квестодателем
/// @param seed    зерно: один и тот же seed даёт один и тот же тип
QuestType       pickType(const QuestGenOptions& options, u64 seed);
u16             pickMobId(world::ChunkManager& world,
                          const glm::ivec3& giverPos,
                          const QuestGenOptions& options, u64 seed);
u16             pickBlockId(world::ChunkManager& world,
                            const glm::ivec3& giverPos, u64 seed);
glm::ivec3      pickExploreLocation(world::ChunkManager& world,
                                    const glm::ivec3& giverPos,
                                    QuestDifficulty diff, u64 seed);

/// Заполнить награды и имя/описание у свежего Quest.
void finalizeQuest(Quest& q,
                   const QuestGenOptions& options,
                   world::ChunkManager& world);

/// Что именно досталось игроку за квест.
///
/// Возвращается наружу, а не пишется в журнал: сдачу квеста ведёт
/// диалог, и показать игроку «+120 золота» должен он же. Молча
/// начислить и промолчать — ровно то, из-за чего золота и не было:
/// никто не замечал, что его нет.
struct GrantedRewards {
    u64 xp          = 0;
    u32 gold        = 0;
    u16 itemId      = 0;   ///< номер ПРЕДМЕТА (не блока)
    u16 itemsToBag  = 0;   ///< сколько влезло в сумку
    u16 itemsToGround = 0; ///< сколько пришлось положить под ноги
    i32 reputation  = 0;

    bool any() const {
        return xp || gold || itemsToBag || itemsToGround || reputation;
    }
};

/// Выдать награду игроку за квест.
///
/// Выдаёт ВСЁ, что обещано в журнале: опыт, репутацию, золото и
/// предмет. Не влезшее в сумку кладётся под ноги отдельной кучкой —
/// награда не имеет права пропасть оттого, что сумка полна.
GrantedRewards grantRewards(ecs::Registry& reg, u32 playerEntity,
                            const Quest& q);

} // namespace quests

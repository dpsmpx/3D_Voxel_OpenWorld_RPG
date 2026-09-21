/**
 * @file quest.h
 * @brief Квесты: шаблоны, процедурная генерация, журнал заданий.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "quest_def.h"
#include <vector>

namespace quests {

/// Журнал квестов. Один на игрока.
struct QuestLog {
    /// Активные квесты (не завершённые). Завершённые удаляются.
    std::vector<ecs::Entity> activeQuests;

    /// Последние N завершённых квестов (для UI истории)
    struct HistoryEntry {
        u32      questId   = 0;
        QuestState state    = QuestState::TurnedIn;
        char     title[64]  = {};
    };
    std::vector<HistoryEntry> history;

    /// API — вызывается из логики игрока и NPC.
    void addActive(ecs::Entity quest);
    void removeActive(ecs::Entity quest);
    void addHistory(u32 questId, QuestState state, const char* title);
    void clear();
};

/// Глобальный счётчик ID квестов. Простой монотонный.
u32 nextQuestId();

// ============================================================
// Работа с прогрессом квестов.
// Все функции возвращают true, если квест был обновлён.
// ============================================================

/// Убийство моба вида mobId.
bool notifyMobKilled(ecs::Registry& reg, u32 playerEntity,
                     u16 mobId, i32 amount = 1);

/// Получение блока/предмета.
/// Привести прогресс «принеси N таких-то» в согласие с сумкой.
///
/// Прогресс таких целей — НЕ счётчик событий, а то, сколько нужного
/// лежит у игрока прямо сейчас. Зовётся каждый кадр; стоит один
/// проход по слотам сумки на активный квест.
///
/// Так было не всегда, и прежний способ врал дважды. Во-первых,
/// счётчик рос от подбора и не падал от траты: журнал показывал
/// «выполнено», когда нужного уже не было. Во-вторых — и это хуже —
/// он сравнивал идентификатор БЛОКА с идентификатором ПРЕДМЕТА, а
/// это разные пространства: блок WOOD равен шести, предмет
/// ITEM_WOOD — пяти. Квест «принести дерева» не двигался от дерева
/// и двигался от листвы, у которой номер предмета случайно совпал с
/// номером блока дерева.
///
/// Здесь пространство одно: сумка спрашивается предметом, полученным
/// из блока через `items::ItemRegistry::blockToItem`.
bool syncCarriedProgress(ecs::Registry& reg, u32 playerEntity);

/// Забрать у игрока то, что он принёс по квесту «принеси».
///
/// Возвращает false, если нужного не хватает: тогда квест не
/// сдаётся. «Принеси» — это обмен, а обмен без передачи предмета
/// обменом не является.
bool consumeCarried(ecs::Registry& reg, u32 playerEntity, const Quest& q);

bool notifyItemCollected(ecs::Registry& reg, u32 playerEntity,
                         u16 blockId, i32 amount = 1);

/// Посещение локации.
bool notifyLocationReached(ecs::Registry& reg, u32 playerEntity,
                           const glm::ivec3& position);

/// Тик квестов с ограничением по времени.
/// Возвращает true, если что-то изменилось.
bool tickQuestTime(ecs::Registry& reg, u32 playerEntity, f32 dt);

/// Проверка: можно ли сдать квест (state == Completed).
bool isReadyToTurnIn(ecs::Registry& reg, ecs::Entity questEntity);

} // namespace quests

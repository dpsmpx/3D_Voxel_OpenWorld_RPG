/**
 * @file dialogue.h
 * @brief NPC: роли, диалоги с ветвлением, поведение жителей.
 */
#pragma once
#include "../core/types.h"
#include "../world/chunk_manager.h"
#include "../ecs/registry.h"
#include "../factions/faction.h"
#include "../quests/quest_def.h"
#include <functional>
#include <vector>
#include <string>

namespace npc {

/// Действие, привязанное к варианту ответа.
enum class DialogueAction : u8 {
    None = 0,
    AcceptQuest,     // взять предложенный квест
    CompleteQuest,   // сдать выполненный квест
    OpenTrade,       // открыть торговлю (Phase 12)
    OpenCraft,       // открыть крафт (Phase 12)
    Heal,            // вылечить игрока за золото
    EndDialogue,
    ReputationCheck, // переход к другой ветке по репутации
};

/// Вариант ответа игрока.
struct DialogueChoice {
    std::string    text;
    DialogueAction action = DialogueAction::None;
    u32            nextNodeId = 0;   // 0 = конец
    // Условия: если не выполнены, вариант скрыт
    factions::FactionId requireFaction = factions::FactionId::None;
    factions::ReputationTier requireMinTier = factions::ReputationTier::Hated;
    u32            requireQuestId = 0;
};

/// Узел диалога — текст от NPC + варианты ответа.
struct DialogueNode {
    u32 id = 0;
    std::string text;
    std::vector<DialogueChoice> choices;

    /// Если true — автоматически перейти по завершении (без вариантов)
    bool autoAdvance = false;
    u32  autoNext    = 0;

    /// Опционально — реакция NPC (анимация, звук). Пока — флаг.
    u8   emote = 0;
};

/// Активный диалог — состояние на игрока.
struct ActiveDialogue {
    bool        active       = false;
    u32         npcEntity    = 0;
    u32         playerEntity = 0;
    u32         currentNodeId = 0;

    /// Список узлов этого диалога — копия из шаблона,
    /// чтобы UI мог обращаться по id.
    std::vector<DialogueNode> nodes;

    /// Quest, предложенный NPC в этом диалоге (если есть).
    u32         offeredQuestEntity = 0;
    u32         offeredQuestId     = 0;

    /// Индекс выбранной опции (для подсветки)
    i32         highlightedChoice = -1;

    /// Действие, которое сам диалог выполнить не может: открыть экран
    /// торговли или крафта. О существовании интерфейса диалог не
    /// знает и знать не должен, поэтому он лишь помечает намерение —
    /// главный цикл его исполняет и сбрасывает.
    ///
    /// Раньше эти две ветки просто закрывали диалог с пометкой
    /// «Phase 12», и выбор «покажи товар» не приводил ни к чему.
    DialogueAction pendingAction = DialogueAction::None;

    DialogueNode* findNode(u32 id) {
        for (auto& n : nodes) if (n.id == id) return &n;
        return nullptr;
    }
};

/// Реестр шаблонов диалогов, привязанных к роли NPC.
/// Строится при старте — статичные тексты.
struct DialogueTemplate {
    std::vector<DialogueNode> nodes;
    u32 rootNodeId = 0;
};

class DialogueRegistry {
public:
    static const DialogueRegistry& instance();
    const DialogueTemplate& get(const char* rootKey) const;

private:
    DialogueRegistry();
    struct Entry {
        std::string key;
        DialogueTemplate tmpl;
    };
    std::vector<Entry> entries_;
};

inline const DialogueRegistry& dialogues() {
    return DialogueRegistry::instance();
}

// ============================================================
// Логика: начать диалог с NPC. Заполняет ActiveDialogue
// у игрока, копируя шаблон и подставляя опции квестов.
// ============================================================
/// Начинает диалог. world нужен генератору квестов: элдер
/// подбирает цель задания по реальному содержимому мира.
bool startDialogue(ecs::Registry& reg,
                   world::ChunkManager& world,
                   u32 playerEntity,
                   u32 npcEntity,
                   const char* dialogueRoot);

/// Применить выбор. Возвращает true, если диалог продолжается.
/// Если choice.action == EndDialogue — закрывает диалог.
bool applyChoice(ecs::Registry& reg,
                 ActiveDialogue& dlg,
                 const DialogueChoice& choice);

/// Закрыть диалог.
void endDialogue(ecs::Registry& reg, u32 playerEntity);

/// Проверка условий для choice.
bool choiceIsAvailable(ecs::Registry& reg,
                       u32 playerEntity,
                       const DialogueChoice& choice,
                       u32 offeredQuestEntity);

} // namespace npc

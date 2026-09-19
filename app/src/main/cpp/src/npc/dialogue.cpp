/**
 * @file dialogue.cpp
 * @brief NPC: роли, диалоги с ветвлением, поведение жителей.
 */
#include "dialogue.h"
#include "npc_def.h"
#include "../ecs/components.h"
#include "../factions/faction.h"
#include "../quests/quest.h"
#include "../quests/quest_generator.h"
#include "../quests/story.h"
#include "../progression/progression.h"
#include "../progression/resource_regen.h"
#include "../items/currency.h"
#include "../core/log.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace npc {

/// Сколько берёт целитель за полное восстановление здоровья.
/// Одно число на текст варианта ответа и на списание — иначе они
/// разъезжаются, что уже однажды и случилось.
constexpr u64 HEAL_COST = 50;


using namespace factions;

// ============================================================
// DialogueRegistry — статические шаблоны
// ============================================================
DialogueRegistry::DialogueRegistry() {
    auto makeEntry = [&](const char* key, DialogueTemplate tmpl) {
        Entry e;
        e.key  = key;
        e.tmpl = std::move(tmpl);
        entries_.push_back(std::move(e));
    };

    // ---------------- villager ----------------
    {
        DialogueTemplate t{};
        t.rootNodeId = 1;

        DialogueNode n1{};
        n1.id = 1;
        n1.text = "Good day, traveller. The village is quiet today.";
        n1.choices.push_back({ "What is this place?", DialogueAction::None, 2 });
        n1.choices.push_back({ "Goodbye.", DialogueAction::EndDialogue, 0 });
        t.nodes.push_back(n1);

        DialogueNode n2{};
        n2.id = 2;
        n2.text = "We are a small settlement. The Elder might have work for you.";
        n2.choices.push_back({ "I'll speak with the Elder.", DialogueAction::EndDialogue, 0 });
        n2.choices.push_back({ "Understood.", DialogueAction::EndDialogue, 0 });
        t.nodes.push_back(n2);

        makeEntry("villager", std::move(t));
    }

    // ---------------- elder (quest giver) ----------------
    {
        DialogueTemplate t{};
        t.rootNodeId = 1;

        DialogueNode n1{};
        n1.id = 1;
        n1.text = "Welcome, adventurer. There is much to do.";
        // Опции будут добавлены при старте диалога динамически.
        n1.choices.push_back({ "What do you need?", DialogueAction::None, 2 });
        n1.choices.push_back({ "Farewell.", DialogueAction::EndDialogue, 0 });
        t.nodes.push_back(n1);

        DialogueNode n2{};
        n2.id = 2;
        n2.text = "Here is what I have for you. Do you accept?";
        // Заполняется в startDialogue
        t.nodes.push_back(n2);

        makeEntry("elder", std::move(t));
    }

    // ---------------- trader ----------------
    {
        DialogueTemplate t{};
        t.rootNodeId = 1;

        DialogueNode n1{};
        n1.id = 1;
        n1.text = "Fine wares, friend. Interested?";
        n1.choices.push_back({ "Show me your wares.", DialogueAction::OpenTrade, 0 });
        n1.choices.push_back({ "Just looking.", DialogueAction::EndDialogue, 0 });
        t.nodes.push_back(n1);

        makeEntry("trader", std::move(t));
    }

    // ---------------- blacksmith ----------------
    {
        DialogueTemplate t{};
        t.rootNodeId = 1;

        DialogueNode n1{};
        n1.id = 1;
        n1.text = "Steel and fire — that's all I need.";
        n1.choices.push_back({ "Can you craft for me?", DialogueAction::OpenCraft, 0 });
        n1.choices.push_back({ "Later.", DialogueAction::EndDialogue, 0 });
        t.nodes.push_back(n1);

        makeEntry("blacksmith", std::move(t));
    }

    // ---------------- guard ----------------
    {
        DialogueTemplate t{};
        t.rootNodeId = 1;

        DialogueNode n1{};
        n1.id = 1;
        n1.text = "Halt. State your business.";
        n1.choices.push_back({ "I mean no harm.", DialogueAction::None, 2 });
        n1.choices.push_back({ "None of yours.", DialogueAction::EndDialogue, 0 });
        t.nodes.push_back(n1);

        DialogueNode n2{};
        n2.id = 2;
        n2.text = "Move along, then. Keep the peace.";
        n2.choices.push_back({ "Understood.", DialogueAction::EndDialogue, 0 });
        t.nodes.push_back(n2);

        makeEntry("guard", std::move(t));
    }

    // ---------------- healer ----------------
    {
        DialogueTemplate t{};
        t.rootNodeId = 1;

        DialogueNode n1{};
        n1.id = 1;
        n1.text = "Wounds of body and mind. I can tend to both.";
        // Цена берётся из одной константы и подставляется в текст:
        // раньше «50 gold» было написано в строке, а списания не было
        // вовсе — целитель лечил бесплатно и сколько угодно раз.
        char healText[64];
        std::snprintf(healText, sizeof(healText), "Heal me (%u gold).",
                      (unsigned)HEAL_COST);
        n1.choices.push_back({ healText, DialogueAction::Heal, 0 });
        n1.choices.push_back({ "Not now.", DialogueAction::EndDialogue, 0 });
        t.nodes.push_back(n1);

        makeEntry("healer", std::move(t));
    }

    LOGI("DialogueRegistry: %zu диалогов", entries_.size());
}

const DialogueRegistry& DialogueRegistry::instance() {
    static DialogueRegistry r;
    return r;
}

const DialogueTemplate& DialogueRegistry::get(const char* rootKey) const {
    if (rootKey) {
        for (const auto& e : entries_) {
            if (e.key == rootKey) return e.tmpl;
        }
    }
    // Возврат пустого, если ключа нет
    static DialogueTemplate empty{};
    return empty;
}

// ============================================================
// Условия вариантов
// ============================================================
bool choiceIsAvailable(ecs::Registry& reg,
                       u32 playerEntity,
                       const DialogueChoice& choice,
                       u32 offeredQuestEntity)
{
    if (choice.requireFaction != FactionId::None) {
        auto* rep = reg.get<Reputation>(playerEntity);
        if (!rep) return false;
        ReputationTier t = rep->tier(choice.requireFaction);
        if ((i8)t < (i8)choice.requireMinTier) return false;
    }

    if (choice.requireQuestId != 0) {
        auto* log = reg.get<quests::QuestLog>(playerEntity);
        if (!log) return false;
        // Проверяем наличие квеста в активных с этим id
        for (auto qe : log->activeQuests) {
            auto* q = reg.get<quests::Quest>(qe);
            if (q && q->id == choice.requireQuestId) return true;
        }
        return false;
    }

    // AcceptQuest — доступно только если у NPC есть предложенный квест
    if (choice.action == DialogueAction::AcceptQuest) {
        if (offeredQuestEntity == 0) return false;
        auto* q = reg.get<quests::Quest>(offeredQuestEntity);
        if (!q) return false;
        if (q->state != quests::QuestState::Available) return false;
    }

    // CompleteQuest — доступно только если есть активный Completed квест
    if (choice.action == DialogueAction::CompleteQuest) {
        auto* log = reg.get<quests::QuestLog>(playerEntity);
        if (!log) return false;
        bool anyReady = false;
        for (auto qe : log->activeQuests) {
            auto* q = reg.get<quests::Quest>(qe);
            if (q && q->giverEntity == 0) continue;
            if (q && q->state == quests::QuestState::Completed) {
                anyReady = true;
                break;
            }
        }
        if (!anyReady) return false;
    }

    return true;
}

// ============================================================
// startDialogue
// ============================================================
bool startDialogue(ecs::Registry& reg,
                   world::ChunkManager& world,
                   u32 playerEntity,
                   u32 npcEntity,
                   const char* dialogueRoot)
{
    auto* active = reg.get<ActiveDialogue>(playerEntity);
    if (!active) return false;

    // Заговорит ли он вообще. RelationModifiers::talksToPlayer
    // существовал с самого начала и не читался нигде: вырезав
    // полдеревни, игрок подходил к следующему жителю и спокойно брал
    // у него квест.
    if (auto* tag = reg.get<NpcTag>(npcEntity)) {
        const NpcDef& ndef = npcRegistry().get(tag->id);
        if (ndef.faction != FactionId::None) {
            if (auto* rep = reg.get<Reputation>(playerEntity)) {
                if (!factions::modifiersFor(rep->tier(ndef.faction)).talksToPlayer)
                    return false;
            }
        }
    }

    const auto& tmpl = dialogues().get(dialogueRoot);
    if (tmpl.nodes.empty()) return false;

    active->active        = true;
    active->npcEntity     = npcEntity;
    active->playerEntity  = playerEntity;
    active->nodes         = tmpl.nodes;
    active->currentNodeId = tmpl.rootNodeId;
    active->highlightedChoice = -1;

    // Обработка элдера: динамически подставляем квесты
    auto* ai = reg.get<NpcAI>(npcEntity);
    auto* npcTag = reg.get<NpcTag>(npcEntity);
    if (ai && npcTag && npcTag->id == NPC_QUEST_GIVER) {
        // Если уже есть предложенный квест — обновляем опции
        // Если нет — генерируем квест
        if (ai->offeredQuest == 0) {
            // Найдём позицию NPC
            glm::ivec3 npcPos{0};
            if (auto* tf = reg.get<ecs::Transform>(npcEntity)) {
                npcPos = { (i32)tf->position.x, (i32)tf->position.y, (i32)tf->position.z };
            }

            u32 playerLevel = 1;
            if (auto* prog = reg.get<progression::Progression>(playerEntity)) {
                playerLevel = prog->level;
            }

            quests::QuestGenOptions opts{};
            opts.giverEntity  = npcEntity;
            opts.ownerEntity  = playerEntity;
            opts.faction      = FactionId::Villagers;
            opts.giverPos     = npcPos;
            opts.playerLevel  = playerLevel;
            opts.allowKill    = true;
            opts.allowCollect = true;
            opts.allowExplore = true;
            // Подсказку к тайнику даёт не всякий: клад помнит тот,
            // кто и так рассказывает истории — раздатчик заданий, а
            // не кузнец у наковальни.
            if (auto* tag = reg.get<NpcTag>(npcEntity))
                opts.allowTreasure =
                    npcRegistry().get(tag->id).role == NpcRole::QuestGiver;
            opts.seed         = (u64)npcEntity * 0xABCDEFULL + playerLevel;

            // Репутация
            if (auto* rep = reg.get<Reputation>(playerEntity)) {
                opts.repTier = rep->tier(FactionId::Villagers);
            }

            // Сюжетная глава идёт ПЕРЕД побочным поручением: если
            // очередная глава готова, раздатчик предлагает её, и
            // только когда цепочка кончилась или её цели нет рядом —
            // обычное поручение.
            ecs::Entity qe{};
            if (auto* tag = reg.get<NpcTag>(npcEntity)) {
                if (npcRegistry().get(tag->id).role == NpcRole::QuestGiver)
                    qe = quests::offerStoryChapter(reg, world,
                                                   reg.fromId(playerEntity),
                                                   npcPos);
            }
            if (!qe.valid()) qe = quests::generateQuest(reg, world, opts);
            ai->offeredQuest = (u32)qe;
        }

        // Обновляем узел 2 — опции про квест
        auto* node = active->findNode(2);
        if (node) {
            node->choices.clear();

            if (auto* q = reg.get<quests::Quest>(ai->offeredQuest)) {
                if (q->state == quests::QuestState::Available) {
                    DialogueChoice accept{};
                    accept.text = "Accept quest.";
                    accept.action = DialogueAction::AcceptQuest;
                    accept.nextNodeId = 0;
                    node->choices.push_back(accept);

                    DialogueChoice decline{};
                    decline.text = "Not now.";
                    decline.action = DialogueAction::EndDialogue;
                    decline.nextNodeId = 0;
                    node->choices.push_back(decline);

                    // Обновим текст узла описанием квеста
                    node->text = q->description;
                }
            }
        }
    }

    // CompleteQuest — всегда добавляем опцию «сдать квест», если есть готовые
    {
        auto* log = reg.get<quests::QuestLog>(playerEntity);
        if (log) {
            bool anyReady = false;
            for (auto qe : log->activeQuests) {
                auto* q = reg.get<quests::Quest>(qe);
                if (q && q->state == quests::QuestState::Completed) {
                    anyReady = true;
                    break;
                }
            }
            if (anyReady) {
                // Добавляем к корневому узлу вариант «сдать квест»
                auto* root = active->findNode(tmpl.rootNodeId);
                if (root) {
                    DialogueChoice c{};
                    c.text   = "I have completed my task.";
                    c.action = DialogueAction::CompleteQuest;
                    c.nextNodeId = 0;
                    root->choices.insert(root->choices.begin(), c);
                }
            }
        }
    }

    return true;
}

// ============================================================
// applyChoice
// ============================================================
bool applyChoice(ecs::Registry& reg,
                 ActiveDialogue& dlg,
                 const DialogueChoice& choice)
{
    switch (choice.action) {

    case DialogueAction::EndDialogue:
        dlg.active = false;
        return false;

    case DialogueAction::AcceptQuest: {
        auto* ai = reg.get<NpcAI>(dlg.npcEntity);
        if (!ai) { dlg.active = false; return false; }
        auto* q = reg.get<quests::Quest>(ai->offeredQuest);
        if (!q) { dlg.active = false; return false; }
        if (q->state != quests::QuestState::Available) {
            dlg.active = false; return false;
        }

        // Переводим квест в Active
        q->state = quests::QuestState::Active;
        q->ownerEntity = dlg.playerEntity;
        q->progress    = 0;

        // Добавляем в журнал игрока
        auto* log = reg.get<quests::QuestLog>(dlg.playerEntity);
        if (log) {
            log->addActive((ecs::Entity)ai->offeredQuest);
        }

        // Очищаем offeredQuest — NPC будет предлагать новый квест
        ai->offeredQuest = 0;
        ai->offerCooldown = 5.f;

        LOGI("Quest accepted: %s (id=%u)", q->title, q->id);

        dlg.active = false;
        return false;
    }

    case DialogueAction::CompleteQuest: {
        auto* log = reg.get<quests::QuestLog>(dlg.playerEntity);
        if (!log) { dlg.active = false; return false; }

        // Находим завершённый квест, сдаём первый подходящий
        for (auto qe : log->activeQuests) {
            auto* q = reg.get<quests::Quest>(qe);
            if (!q) continue;
            if (q->state != quests::QuestState::Completed) continue;

            // Награда
            quests::grantRewards(reg, dlg.playerEntity, *q);

            // Сюжет двигается ТОЛЬКО сдачей: дойти до места мало,
            // надо вернуться и рассказать. Иначе следующая глава
            // выдавалась бы игроку прямо посреди леса.
            if (q->isStory)
                quests::completeStoryChapter(reg,
                                             reg.fromId(dlg.playerEntity),
                                             q->id);

            // Обновляем состояние и историю
            q->state = quests::QuestState::TurnedIn;
            log->addHistory(q->id, q->state, q->title);
            log->removeActive(qe);

            LOGI("Quest turned in: %s", q->title);

            // Удалить сущность квеста
            reg.destroy(qe);
            break;
        }

        dlg.active = false;
        return false;
    }

    case DialogueAction::OpenTrade:
    case DialogueAction::OpenCraft:
        // Экран открывает главный цикл: диалог не знает об интерфейсе.
        dlg.pendingAction = choice.action;
        dlg.active = false;
        return false;

    case DialogueAction::Heal: {
        // В самом варианте ответа написано «за 50 золотых», а списания
        // не было: целитель лечил бесплатно и сколько угодно раз.
        auto* h   = reg.get<ecs::Health>(dlg.playerEntity);
        auto* wal = reg.get<items::Wallet>(dlg.playerEntity);
        if (h && wal && h->current < h->max && wal->spend(HEAL_COST)) {
            h->current = h->max;
            LOGI("Целитель: восстановил здоровье за %u золотых",
                 (unsigned)HEAL_COST);
        }
        dlg.active = false;
        return false;
    }

    case DialogueAction::None:
    default:
        if (choice.nextNodeId != 0) {
            dlg.currentNodeId = choice.nextNodeId;
            dlg.highlightedChoice = -1;
            return true;
        }
        dlg.active = false;
        return false;
    }
}

void endDialogue(ecs::Registry& reg, u32 playerEntity) {
    auto* dlg = reg.get<ActiveDialogue>(playerEntity);
    if (dlg) {
        dlg->active = false;
    }
}

} // namespace npc

/**
 * @file quest_text.cpp
 * @brief Квесты: шаблоны, процедурная генерация, журнал заданий.
 */
#include "quest_def.h"
#include "story.h"
#include "../config/localization.h"
#include "../items/item_def.h"
#include "../mobs/mob_def.h"
#include <string>

namespace quests {

namespace {

/// Текст двух первых целей.
///
/// Здесь, а не в story.cpp, по той же причине, по какой здесь лежит
/// и всё остальное: текст задания собирается в одном месте, и чтобы
/// узнать, что увидит игрок, не нужно ходить по трём файлам.
constexpr const char* FIRST_VILLAGE_TITLE = "Find people";
constexpr const char* FIRST_VILLAGE_DESC  =
    "There is a village somewhere near. Reach it and talk to "
    "whoever lives there.";
constexpr const char* FIRST_WOOD_TITLE = "The first tree";
constexpr const char* FIRST_WOOD_DESC  =
    "Nobody is in sight. Gather wood: both the axe and everything "
    "after it start with it.";

/// На какой глубине замурован тайник. Одно число на генератор ям и
/// на строчку «закопано на N блоков вглубь».
constexpr i32 TREASURE_DEPTH = 6;

const char* firstStepTitle(QuestTextSource s) {
    return s == QuestTextSource::FirstStepVillage ? FIRST_VILLAGE_TITLE
                                                  : FIRST_WOOD_TITLE;
}
const char* firstStepDesc(QuestTextSource s) {
    return s == QuestTextSource::FirstStepVillage ? FIRST_VILLAGE_DESC
                                                  : FIRST_WOOD_DESC;
}

} // namespace

const char* blockTargetName(u16 blockId) {
    const u16 itemId = items::items().blockToItem(blockId);
    if (itemId == items::ITEM_NONE) return config::tr("materials");
    return items::items().name(itemId);
}

std::string questTitle(const Quest& q) {
    switch (q.textSource) {
        case QuestTextSource::Story:
            return config::tr(storyChapter(q.storyChapter).title);
        case QuestTextSource::FirstStepVillage:
        case QuestTextSource::FirstStepWood:
            return config::tr(firstStepTitle(q.textSource));
        default: break;
    }

    const auto& def = questTemplates().get(q.tmpl.type, q.tmpl.difficulty);

    // Имя подставляется только туда, где шаблон его ждёт. У «дойти»,
    // «докопаться», «защитить» и «отнести» цель — место или срок, а
    // не имя, и названия у них без подстановок вовсе.
    switch (q.tmpl.type) {
        case QuestType::Kill:
            return config::trf(def.titlePattern,
                               mobs::mobRegistry().name(q.tmpl.targetMobId));
        case QuestType::Collect:
            return config::trf(def.titlePattern,
                               blockTargetName(q.tmpl.targetBlockId));
        default:
            return config::tr(def.titlePattern);
    }
}

std::string questDescription(const Quest& q) {
    switch (q.textSource) {
        case QuestTextSource::Story: {
            // К рассказу главы дописывается то, что от игрока
            // требуется на деле: сколько бить или куда идти. Текст
            // главы — это история, а не задание, и без этой строки
            // игрок читает красивое и не понимает, что ему делать.
            std::string s = config::tr(storyChapter(q.storyChapter).text);
            s += ' ';
            if (q.tmpl.type == QuestType::Kill)
                s += config::trf("Need %d.", q.tmpl.requiredCount);
            else
                s += config::trf("Head for (%d, %d).",
                                 q.tmpl.targetLocation.x,
                                 q.tmpl.targetLocation.z);
            return s;
        }
        case QuestTextSource::FirstStepVillage:
        case QuestTextSource::FirstStepWood:
            return config::tr(firstStepDesc(q.textSource));
        default: break;
    }

    const auto& def = questTemplates().get(q.tmpl.type, q.tmpl.difficulty);
    const i32 count = q.tmpl.requiredCount;

    switch (q.tmpl.type) {
        case QuestType::Kill:
            return config::trf(def.descPattern,
                               mobs::mobRegistry().name(q.tmpl.targetMobId),
                               count);
        case QuestType::Collect:
            return config::trf(def.descPattern,
                               blockTargetName(q.tmpl.targetBlockId),
                               count);
        case QuestType::Defend:
            return config::trf(def.descPattern, count);

        case QuestType::Explore: {
            // Координаты цели — отдельной фразой, а не частью
            // шаблона: у всех пяти сложностей они одни и те же, и
            // повторять их в пяти строках значило бы завести пять
            // мест, где можно ошибиться.
            std::string s = config::tr(def.descPattern);
            s += ' ';
            s += config::trf("Target: (%d, %d, %d).",
                             q.tmpl.targetLocation.x,
                             q.tmpl.targetLocation.y,
                             q.tmpl.targetLocation.z);
            return s;
        }
        case QuestType::Treasure: {
            // Глубину называем отдельно: без неё игрок стоит на
            // нужном месте и не понимает, что копать надо вниз.
            std::string s = config::tr(def.descPattern);
            s += ' ';
            s += config::trf("Ruins at (%d, %d), buried %d blocks down.",
                             q.tmpl.targetLocation.x,
                             q.tmpl.targetLocation.z,
                             TREASURE_DEPTH);
            return s;
        }
        default:
            return config::tr(def.descPattern);
    }
}

} // namespace quests

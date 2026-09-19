/**
 * @file quest_def.cpp
 * @brief Квесты: шаблоны, процедурная генерация, журнал заданий.
 */
#include "quest_def.h"
#include "../core/log.h"
#include <cstring>

namespace quests {

QuestTemplateRegistry::QuestTemplateRegistry() {
    auto set = [&](QuestType t, QuestDifficulty d,
                   const char* title, const char* desc,
                   i32 minC, i32 maxC,
                   u64 xp, u32 gold, i32 rep)
    {
        auto& def = defs[(u8)t][(u8)d];
        def.type = t;
        def.difficulty = d;
        def.titlePattern = title;
        def.descPattern  = desc;
        def.minCount = minC;
        def.maxCount = maxC;
        def.baseXP = xp;
        def.baseGold = gold;
        def.baseRep = rep;
    };

    // ---------------- Kill ----------------
    set(QuestType::Kill, QuestDifficulty::Trivial,
        "Cull the %s",    "Slay %d %s near the village.",
        3, 5, 60, 10, 5);
    set(QuestType::Kill, QuestDifficulty::Easy,
        "Clear %s",       "Cull %d %s from the surrounding lands.",
        5, 8, 150, 25, 10);
    set(QuestType::Kill, QuestDifficulty::Normal,
        "Hunt %s",        "The village needs %d %s slain.",
        8, 12, 400, 60, 20);
    set(QuestType::Kill, QuestDifficulty::Hard,
        "Purge the %s",   "Eliminate %d %s. Beware their numbers.",
        12, 18, 900, 120, 35);
    set(QuestType::Kill, QuestDifficulty::Epic,
        "Extermination: %s","Only the strongest dare face %d %s.",
        20, 30, 2200, 300, 60);

    // ---------------- Collect ----------------
    set(QuestType::Collect, QuestDifficulty::Trivial,
        "Gather %d %s",   "Bring %d %s to the village elder.",
        5, 10, 80, 15, 5);
    set(QuestType::Collect, QuestDifficulty::Easy,
        "Supply %s",      "The craftsmen need %d %s.",
        10, 20, 180, 30, 10);
    set(QuestType::Collect, QuestDifficulty::Normal,
        "Stockpile %s",   "A bulk order: %d %s required.",
        20, 40, 450, 70, 20);
    set(QuestType::Collect, QuestDifficulty::Hard,
        "Bulk %s",        "Rare materials: %d %s. Take care.",
        40, 70, 1000, 150, 35);
    set(QuestType::Collect, QuestDifficulty::Epic,
        "Tribute of %s",  "An epic demand: %d %s for the cause.",
        80, 120, 2400, 320, 60);

    // ---------------- Explore ----------------
    set(QuestType::Explore, QuestDifficulty::Trivial,
        "Scout nearby",   "Explore the area near the village.",
        1, 1, 40, 8, 3);
    set(QuestType::Explore, QuestDifficulty::Easy,
        "Scout the ruins","Locate the old ruins to the %s.",
        1, 1, 120, 20, 8);
    set(QuestType::Explore, QuestDifficulty::Normal,
        "Chart %s",       "Reach the coordinates shown on your map.",
        1, 1, 350, 55, 15);
    set(QuestType::Explore, QuestDifficulty::Hard,
        "Expedition",     "Travel far from safety to explore.",
        1, 1, 800, 110, 30);
    set(QuestType::Explore, QuestDifficulty::Epic,
        "Beyond the veil","A legendary journey awaits.",
        1, 1, 2000, 280, 55);

    // ---------------- Treasure ----------------
    //
    // Награда меньше, чем у Explore той же сложности: главная
    // награда тут в самой яме.
    set(QuestType::Treasure, QuestDifficulty::Trivial,
        "A rumour",       "Old folk speak of a cache beneath the ruins.",
        1, 1, 60, 10, 3);
    set(QuestType::Treasure, QuestDifficulty::Easy,
        "Buried under stone", "Dig down at the ruins and see what is there.",
        1, 1, 150, 24, 8);
    set(QuestType::Treasure, QuestDifficulty::Normal,
        "The sealed vault", "A walled chamber lies under the old ruins.",
        1, 1, 380, 60, 15);
    set(QuestType::Treasure, QuestDifficulty::Hard,
        "Far cache",      "The cache is far, and nobody has dug it out yet.",
        1, 1, 850, 120, 30);
    set(QuestType::Treasure, QuestDifficulty::Epic,
        "The last hoard", "A hoard nobody has reached in living memory.",
        1, 1, 2100, 300, 55);

    // ---------------- Defend ----------------
    set(QuestType::Defend, QuestDifficulty::Trivial,
        "Hold the line",  "Protect the villager for %d seconds.",
        20, 30, 100, 15, 5);
    set(QuestType::Defend, QuestDifficulty::Easy,
        "Guard duty",     "Defend for %d seconds.",
        30, 45, 200, 30, 10);
    set(QuestType::Defend, QuestDifficulty::Normal,
        "Siege defense",  "Survive a %d-second assault.",
        45, 60, 500, 80, 20);
    set(QuestType::Defend, QuestDifficulty::Hard,
        "Last stand",     "Hold out for %d seconds against all odds.",
        60, 90, 1200, 180, 40);
    set(QuestType::Defend, QuestDifficulty::Epic,
        "Legendary guard","Protect for %d seconds. No mercy.",
        90, 150, 2600, 400, 70);

    // ---------------- Deliver ----------------
    set(QuestType::Deliver, QuestDifficulty::Trivial,
        "Delivery",       "Deliver the package to the recipient.",
        1, 1, 60, 12, 4);
    set(QuestType::Deliver, QuestDifficulty::Easy,
        "Special delivery","Transport the goods safely.",
        1, 1, 150, 25, 8);
    set(QuestType::Deliver, QuestDifficulty::Normal,
        "Courier run",    "Deliver through dangerous territory.",
        1, 1, 400, 65, 18);
    set(QuestType::Deliver, QuestDifficulty::Hard,
        "Urgent dispatch","Hurry — the recipient is far away.",
        1, 1, 900, 130, 32);
    set(QuestType::Deliver, QuestDifficulty::Epic,
        "Legendary courier","A delivery that will be remembered.",
        1, 1, 2100, 300, 58);

    LOGI("QuestTemplateRegistry: %u шаблонов",
         (unsigned)((u8)QuestType::Count * (u8)QuestDifficulty::Count));
}

const QuestTemplateRegistry& QuestTemplateRegistry::instance() {
    static QuestTemplateRegistry r;
    return r;
}

const QuestTemplateDef& QuestTemplateRegistry::get(QuestType t, QuestDifficulty d) const {
    u8 ti = (u8)t;
    u8 di = (u8)d;
    if (ti >= (u8)QuestType::Count) ti = 0;
    if (di >= (u8)QuestDifficulty::Count) di = 0;
    return defs[ti][di];
}

const char* questTypeName(QuestType t) {
    switch (t) {
        case QuestType::Kill:    return "Kill";
        case QuestType::Collect: return "Collect";
        case QuestType::Explore: return "Explore";
        case QuestType::Defend:  return "Defend";
        case QuestType::Deliver: return "Deliver";
        case QuestType::Treasure: return "Treasure";
        default:                 return "?";
    }
}

const char* questDifficultyName(QuestDifficulty d) {
    switch (d) {
        case QuestDifficulty::Trivial: return "Trivial";
        case QuestDifficulty::Easy:    return "Easy";
        case QuestDifficulty::Normal:  return "Normal";
        case QuestDifficulty::Hard:    return "Hard";
        case QuestDifficulty::Epic:    return "Epic";
        default:                       return "?";
    }
}

u32 questDifficultyValue(QuestDifficulty d) {
    switch (d) {
        case QuestDifficulty::Trivial: return 1;
        case QuestDifficulty::Easy:    return 2;
        case QuestDifficulty::Normal:  return 3;
        case QuestDifficulty::Hard:    return 4;
        case QuestDifficulty::Epic:    return 5;
        default:                       return 1;
    }
}

} // namespace quests

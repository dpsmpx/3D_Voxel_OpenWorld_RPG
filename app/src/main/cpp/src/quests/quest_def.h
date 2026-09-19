/**
 * @file quest_def.h
 * @brief Квесты: шаблоны, процедурная генерация, журнал заданий.
 */
#pragma once
#include "../core/types.h"
#include "../world/block.h"
#include "../mobs/mob_def.h"
#include "../factions/faction.h"

namespace quests {

/// Типы квестов.
enum class QuestType : u8 {
    Kill = 0,       // убить N мобов вида X
    Collect,        // принести N блоков/предметов Y
    Explore,        // посетить локацию Z (координаты + радиус)
    Defend,         // защитить NPC в течение N секунд
    Deliver,        // отнести предмет NPC
    /// Докопаться до замурованного тайника под руинами.
    ///
    /// Отдельный тип, а не Explore с другими словами: у Explore
    /// цель — любая проходимая точка в стороне, а здесь цель —
    /// настоящий клад, который в мире действительно лежит. Смешать
    /// их значило бы отправить игрока копать там, где ничего нет.
    Treasure,
    Count
};

/// Сложность влияет на количество, награду и уровень.
enum class QuestDifficulty : u8 {
    Trivial = 0,
    Easy,
    Normal,
    Hard,
    Epic,
    Count
};

/// Состояние квеста.
enum class QuestState : u8 {
    Available = 0,  // доступен у NPC, ещё не взят
    Active,         // взят, в процессе
    Completed,      // требования выполнены, ждёт сдачи
    TurnedIn,       // сдан, награда получена
    Failed,         // провален (NPC умер, время вышло)
    Abandoned,      // игрок отказался
};

/// Награды.
struct QuestRewards {
    u64 xp              = 0;
    u32 gold            = 0;
    u16 itemBlockId     = 0;   // 0 — нет предмета
    u8  itemCount       = 0;
    i32 reputationDelta = 0;   // к союзной фракции
    factions::FactionId reputationFaction = factions::FactionId::None;
};

/// Шаблон квеста — генерируется один раз и не меняется.
struct QuestTemplate {
    QuestType       type;
    QuestDifficulty difficulty;
    u16             targetMobId    = 0;       // для Kill
    u16             targetBlockId  = 0;       // для Collect
    glm::ivec3      targetLocation {0};       // для Explore / Defend
    i32             targetRadius   = 0;
    i32             requiredCount  = 0;
    f32             timeLimit      = 0.f;     // 0 = без ограничения
    factions::FactionId giverFaction = factions::FactionId::None;
};

/// Компонент квеста — ECS-сущность с этим компонентом
/// представляет активный квест в журнале игрока.
struct Quest {
    u32            id            = 0;    // уникальный идентификатор квеста
    QuestTemplate  tmpl{};
    QuestState     state         = QuestState::Available;
    i32            progress      = 0;
    f32            timeRemaining = 0.f;

    u32            giverEntity   = 0;    // NPC, выдавший квест
    u32            ownerEntity   = 0;    // игрок, взявший квест

    /// Для Defend
    u32            defendTarget  = 0;
    f32            defendTimer   = 0.f;

    /// Награда (уже скалькулирована при генерации)
    QuestRewards   rewards{};

    // Отображаемое имя и описание
    char           title[64]     = {};
    char           description[192] = {};

    bool isComplete() const {
        return state == QuestState::Completed;
    }
    bool isActive() const {
        return state == QuestState::Active;
    }
    bool isDone() const {
        return state == QuestState::TurnedIn ||
               state == QuestState::Failed ||
               state == QuestState::Abandoned;
    }

    f32 progressPct() const {
        if (tmpl.requiredCount <= 0) return 1.f;
        f32 p = (f32)progress / (f32)tmpl.requiredCount;
        if (p < 0.f) p = 0.f;
        if (p > 1.f) p = 1.f;
        return p;
    }
};

/// Реестр шаблонов-заготовок для процедурной генерации.
struct QuestTemplateDef {
    QuestType type;
    QuestDifficulty difficulty;
    const char* titlePattern;    // "Slay %d %s"
    const char* descPattern;
    i32 minCount;
    i32 maxCount;
    u64 baseXP;
    u32 baseGold;
    i32 baseRep;
};

class QuestTemplateRegistry {
public:
    static const QuestTemplateRegistry& instance();
    const QuestTemplateDef& get(QuestType t, QuestDifficulty d) const;

private:
    QuestTemplateRegistry();
    QuestTemplateDef defs[(u8)QuestType::Count][(u8)QuestDifficulty::Count];
};

inline const QuestTemplateRegistry& questTemplates() {
    return QuestTemplateRegistry::instance();
}

/// Утилиты
const char* questTypeName(QuestType t);
/// Название сложности квеста для журнала заданий.
const char* questDifficultyName(QuestDifficulty d);
/// Числовой вес сложности — множитель награды и требований.
u32         questDifficultyValue(QuestDifficulty d);

} // namespace quests

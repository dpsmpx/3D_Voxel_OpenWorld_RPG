/**
 * @file quest_def.h
 * @brief Квесты: шаблоны, процедурная генерация, журнал заданий.
 */
#pragma once
#include "../core/types.h"
#include "../world/block.h"
#include "../mobs/mob_def.h"
#include "../factions/faction.h"
#include <string>

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

/// Откуда задание берёт своё название и описание.
///
/// Generated — шаблон по типу и сложности плюс имя цели и число;
/// Story — глава сюжетной цепочки по номеру; две первых цели стоят
/// отдельно, потому что их текст ни на что не похож: их выдаёт не
/// раздатчик, а сама игра в первую минуту.
enum class QuestTextSource : u8 {
    Generated = 0,
    Story,
    FirstStepVillage,
    FirstStepWood,
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

    /// Откуда берётся текст задания.
    ///
    /// Текста в самом задании больше нет. Раньше здесь лежали
    /// `char title[64]` и `char description[192]` — слепленные при
    /// выдаче и с тех пор неизменные. Из-за этого название задания
    /// нельзя было перевести: смена языка меняла обвязку
    /// интерфейса, а журнал оставался на языке, который был в силе
    /// в момент выдачи, и уезжал в сохранение.
    ///
    /// Теперь задание помнит, ОТКУДА взять слова, и собирает их в
    /// момент показа — см. questTitle() и questDescription(). Заодно
    /// ушли двести пятьдесят шесть байт текста с каждого задания и
    /// из каждой записи истории.
    QuestTextSource textSource   = QuestTextSource::Generated;
    u8             storyChapter  = 0;

    u32            giverEntity   = 0;    // NPC, выдавший квест
    u32            ownerEntity   = 0;    // игрок, взявший квест

    /// Для Defend
    u32            defendTarget  = 0;
    f32            defendTimer   = 0.f;

    /// Награда (уже скалькулирована при генерации)
    QuestRewards   rewards{};

    /// Сюжетная глава, а не побочное поручение.
    ///
    /// Отличать их нужно и журналу, и сдаче: побочных квестов игрок
    /// берёт сколько хочет и в любом порядке, а глава одна и идёт
    /// строго за предыдущей. Отдельного поля для этого нет: главу
    /// от поручения отличает источник текста, и держать рядом ещё и
    /// флаг значило бы завести два признака одного и того же.
    bool isStory() const { return textSource == QuestTextSource::Story; }

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

/// Название задания на языке игрока.
///
/// Собирается здесь и сейчас: шаблон берётся по типу и сложности,
/// имя цели — из реестра тварей или предметов, и всё это проходит
/// через словарь содержимого. Поэтому переключение языка меняет и
/// журнал, и строку текущего задания, и историю — без перевыдачи
/// заданий и без правки сохранения.
std::string questTitle(const Quest& q);

/// Описание задания на языке игрока. См. questTitle().
///
/// «Дойти» и «докопаться» дописывают к описанию координаты цели:
/// без них игрок знает, что надо куда-то дойти, и не знает куда.
std::string questDescription(const Quest& q);

/// Имя цели задания «принеси»: как называется этот блок.
///
/// Через предмет, который из блока получается: имён у блоков нет, а
/// у предметов есть, и в сумке игрок видит именно их. Раньше здесь
/// стояла таблица из четырнадцати английских слов прямо в
/// генераторе — всё, что за четырнадцатым блоком, называлось
/// «materials».
const char* blockTargetName(u16 blockId);

/// Утилиты
const char* questTypeName(QuestType t);
/// Название сложности квеста для журнала заданий.
const char* questDifficultyName(QuestDifficulty d);
/// Числовой вес сложности — множитель награды и требований.
u32         questDifficultyValue(QuestDifficulty d);

} // namespace quests

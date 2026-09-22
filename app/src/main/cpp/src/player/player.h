/**
 * @file player.h
 * @brief Игрок: ввод, движение, взаимодействие с миром и NPC.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../physics/character_controller.h"
#include "../physics/raycast.h"
#include "../world/chunk_manager.h"
#include "../world/features.h"
#include "../combat/components.h"
#include "../combat/weapon.h"
#include "../combat/resonance.h"
#include "../combat/combat_controller.h"
#include "../combat/guard.h"
#include "../combat/spatial_hash.h"
#include "../progression/progression.h"
#include "../progression/skill_tree.h"
#include "../quests/quest.h"
#include "../factions/faction.h"
#include "../npc/dialogue.h"
#include "../items/inventory.h"
#include "../items/currency.h"
#include "../items/item_use.h"
#include <glm/glm.hpp>

namespace player {

enum class CameraMode : u8 {
    ThirdPerson,
    FirstPerson,
};

struct PlayerInput {
    glm::vec2 moveAxis{0};
    bool jumpPressed = false;
    bool jumpHeld    = false;
    bool sprint      = false;
    bool crouch      = false;

    bool attackPressed = false;
    bool attackHeld    = false;
    bool finisherInput = false;
    bool dashPressed   = false;
    /// Защита — УДЕРЖАНИЕ, а не нажатие: поднял и держишь, пока
    /// нужно. Момент подъёма при этом решает всё (см. PARRY_WINDOW),
    /// поэтому важно именно удержание, а не переключатель.
    bool blockHeld     = false;

    bool interactPressed = false;
};

/// Сколько секунд игрок лежит мёртвым, прежде чем вернуться.
///
/// Не ноль: мгновенный возврат не даёт понять, что случилось, —
/// экран просто прыгает. И не десять: это наказание временем, а не
/// игра.
constexpr f32 DEATH_DELAY = 2.5f;

/// На каком расстоянии от колодца он становится точкой возрождения.
///
/// Колодец — пять блоков в поперечнике, поэтому четыре: подойти надо
/// вплотную, но не выцеливать центральную клетку.
constexpr f32 WELL_TOUCH_DIST = 4.f;

/// Сколько выносливости стоит рывок.
///
/// Двадцать пять из ста: четыре рывка подряд на полной шкале, дальше
/// придётся отдышаться. Без цены рывок заменил бы ходьбу.
constexpr f32 DASH_STAMINA = 25.f;

/// ---- Бег ----
///
/// Двенадцать в секунду из ста: восемь секунд бега на полной шкале,
/// а с учётом регена — около десяти. Достаточно, чтобы убежать от
/// стаи или догнать посыльного, и мало, чтобы пересечь бегом весь
/// мир, ни разу не сбавив шаг.
constexpr f32 SPRINT_STAMINA_PER_SEC = 12.f;

/// ---- Падение ----
///
/// Обрыва в игре не было, потому что падать было не больно: с любой
/// высоты игрок приземлялся целым, и пропасть работала лифтом вниз.
///
/// Четыре блока без вреда — это высота, с которой спрыгивают, не
/// задумываясь: выход из дома, уступ, край дорожки.
constexpr f32 FALL_SAFE_BLOCKS = 4.f;

/// Урона за каждый блок сверх безопасного.
///
/// Восемь: падение с десяти блоков снимает половину сотни, с
/// семнадцати — убивает. Ровно то, что делает обрыв обрывом.
constexpr f32 FALL_DAMAGE_PER_BLOCK = 8.f;

/// ---- Дыхание ----
///
/// Двадцать секунд под водой: хватит доплыть до дна озера и обратно,
/// и не хватит жить там.
constexpr f32 BREATH_SECONDS = 20.f;

/// За сколько секунд на воздухе запас восстанавливается целиком.
/// Быстро: это не наказание, а то, ради чего выныривают.
constexpr f32 BREATH_REFILL_SECONDS = 4.f;

/// Урон за глоток воды и как часто он приходит.
constexpr f32 DROWN_DAMAGE   = 6.f;
constexpr f32 DROWN_INTERVAL = 1.f;

/// До какого запаса надо отдышаться, чтобы бег включился снова.
/// Без этого порога бег дрожал бы: выносливость капнула — кнопка
/// сработала — выносливость снова ноль, и так каждый кадр.
constexpr f32 SPRINT_RESUME = 25.f;

class Player {
public:
    void init(ecs::Registry& reg, const glm::vec3& spawnPos);

    /// Базовый апдейт без spatial hash (fallback).
    void update(world::ChunkManager& world,
                const PlayerInput& input,
                f32 dt,
                f32 cameraYaw,
                f32 cameraPitch);

    /// Phase 15: апдейт с spatial hash — используется в main.
    /// hash может быть nullptr — тогда падаем на полный перебор.
    void updateWithHash(world::ChunkManager& world,
                        const combat::SpatialHash* hash,
                        const PlayerInput& input,
                        f32 dt,
                        f32 cameraYaw,
                        f32 cameraPitch);

    glm::vec3 eyePosition() const;
    glm::vec3 centerPosition() const;

    physics::RayHit targetBlock(world::ChunkManager& world, f32 reach = 5.5f) const;
    glm::vec3 aimDir() const { return aimDir_; }

    /// Ломает блок под прицелом и роняет из него предмет.
    bool tryBreakBlock(world::ChunkManager& world);
    /// Ставит блок и списывает его из активной ячейки пояса.
    bool tryPlaceBlock(world::ChunkManager& world, u16 blockType);

    ecs::Entity entity() const { return entity_; }

    /// Прямой доступ к реестру — нужен UI и другим системам.
    ecs::Registry* registryHandle() const { return reg_; }

    progression::Progression* progression();
    progression::SkillTree* skillTree();
    ecs::Attributes* attributes();
    quests::QuestLog* questLog();
    factions::Reputation* reputation();
    npc::ActiveDialogue* activeDialogue();

    items::Inventory* inventory();
    items::Wallet* wallet();

    const progression::DerivedStats& derived() const;

    /// Принять текущую репутацию за исходную, ничего не объявляя.
    ///
    /// Нужно после загрузки: там тиры меняются разом и не игроком, а
    /// экран «репутация у такой-то фракции теперь такая» сообщал бы о
    /// том, что случилось в прошлой жизни.
    void resyncReputationBaseline();

    void tryInteract(ecs::Registry& reg, world::ChunkManager& world);

    /// Применить предмет. Метательное бросается по направлению
    /// взгляда — для этого и нужен мир.
    items::UseResult useItem(world::ChunkManager& world, u32 slotIndex);
    bool dropItem(u32 slotIndex);

    void setActiveHotbar(u8 idx);
    u16 selectedBlock() const;

    /// Камера
    CameraMode cameraMode = CameraMode::ThirdPerson;
    f32 thirdPersonDistance = 5.5f;
    f32 thirdPersonHeight   = 1.10f;
    f32 firstPersonEye      = 1.62f;

    f32 bobPhase  = 0.f;
    f32 bobAmount = 0.f;

    physics::CharacterController controller;

    combat::EquippedWeapon   equipped;
    combat::WeaponState      weaponState;
    combat::ResonanceState   resonance;
    combat::StatusEffects    statuses;
    combat::Combatant        combatant;
    combat::CombatAction     lastAction;

    /// Подсказку про парирование показываем один раз за сессию — и
    /// только после того, как игрок сам что-нибудь заблокировал.
    /// Объяснять механику до того, как она встретилась, некому.
    ///
    /// Копии самого GuardState здесь нет намеренно: рисует защиту
    /// рендер прямо по компоненту, а интерфейсу показывать нечего —
    /// поднятые руки видны на модели, а цену блока показывает полоса
    /// выносливости. Поле, в которое пишут и которое не читают, —
    /// ровно то, за чем в этом проекте следят.
    bool pendingParryHint = false;

    bool pendingLevelUpNotification = false;
    f32  levelUpFlashTimer = 0.f;
    u32  lastLevelGained   = 1;

    /// ---- Смерть и возвращение ----
    ///
    /// Смерти в игре не было вовсе: здоровье уходило в ноль, звучал
    /// звук, и на этом всё — игрок оставался стоять с нулём.
    bool newRespawnPoint = false; ///< поднят, когда колодец запомнен

    /// ---- Логова ----
    ///
    /// Игрок, вошедший в волчье логово, видит только волков. Без
    /// слова об этом происходящее читается как поломка спавна, а не
    /// как место со своим нравом.
    /// Выдохся ли: бег не включается, пока не отдышится. Спрашивают
    /// снаружи — интерфейсу надо гасить кнопку бега, а не оставлять
    /// её нажатой и бездействующей.
    bool winded() const { return winded_; }

    /// Захлебнулись на этом кадре: спрашивают снаружи, чтобы сказать
    /// игроку — иначе здоровье тает молча.
    bool justDrowned = false;

    /// Сколько урона снято последним падением. Ноль — обошлось.
    /// Спрашивают снаружи: об ударе о землю игроку надо сказать.
    f32 lastFallDamage = 0.f;

    /// Вошли в деревню нового уклада. Поднят на кадре входа.
    bool enteredVillage = false;
    world::VillageStyle villageStyle = world::VillageStyle::Count;

    bool enteredLair = false;             ///< поднят на кадре входа
    world::LairKind lairKind = world::LairKind::None; ///< где стоим
    bool justDied      = false;   ///< поднят на кадре смерти
    /// Сколько золота осталось на месте гибели. Нужно кадру
    /// смерти, чтобы сказать игроку цену вслух.
    u64  lostGold      = 0;
    bool justRespawned = false;   ///< поднят на кадре возвращения
    f32  deathTimer    = 0.f;     ///< сколько лежать осталось
    bool dead          = false;

    bool reputationFlashActive = false;
    f32  reputationFlashTimer  = 0.f;
    factions::FactionId lastRepFaction = factions::FactionId::None;
    factions::ReputationTier lastRepTier = factions::ReputationTier::Neutral;

    u16 legacySelectedBlock = world::STONE;

    /// ---- Отдача камеры ----
    ///
    /// Копится здесь, тратится камерой. Игрок — единственное место,
    /// которое уже знает и о своём попадании, и о своей боли, и о
    /// падении с обрыва; собирать эти три события в рендере значило
    /// бы дублировать их в третий раз.
    ///
    /// @return накопленную величину и обнуляет её.
    f32 takeCameraShake() {
        const f32 v = cameraShake_;
        cameraShake_ = 0.f;
        return v;
    }

    /// Принять текущее здоровье за исходное, не считая разницу
    /// уроном. Нужно после загрузки: здоровье из сейва — не событие
    /// этого кадра, и трясти камеру за урон прошлой жизни незачем.
    /// Ровно та же причина, что у resyncReputationBaseline().
    void resyncImpactBaseline();

private:
    /// Насколько тряхнуть камеру в этом кадре, 0..1.
    f32 cameraShake_ = 0.f;

    /// Подсказку про парирование показывали. Живёт сессию и в сейв
    /// не идёт: вреда от второго показа в новой сессии нет, а место
    /// в формате сохранения он бы занял навсегда.
    bool parryHintShown_ = false;

    /// Здоровье на прошлом кадре. Иначе «сколько с меня сняли» не
    /// узнать: одним числом ecs::Health хранит только остаток.
    ///
    /// Одна точка на ВСЕ источники урона — тварь, яд, падение,
    /// утопление, лава. Ловить их поимённо значило бы завести пятое
    /// место, где перечислено, отчего игроку бывает больно.
    f32 prevHealth_ = -1.f;

    /// Накопить отдачу от того, что случилось за кадр.
    void noticeImpacts();

    /// Общая реализация для update / updateWithHash.
    void updateImpl(world::ChunkManager& world,
                    const combat::SpatialHash* hash,
                    const PlayerInput& input,
                    f32 dt,
                    f32 cameraYaw,
                    f32 cameraPitch);

    /// Списать один поставленный блок из активной ячейки пояса.
    void consumePlacedBlock(u16 blockType);

    /// Заметить смену тира репутации и поднять уведомление.
    void noticeReputationChange();

    /// Запомнить колодец под ногами как точку возвращения.
    void noticeWell(world::ChunkManager& world);

    /// Заметить, что вошли в логово (или вышли из него).
    void noticeLair(world::ChunkManager& world);

    /// Заметить, в деревню какого уклада вошли.
    void noticeVillage(world::ChunkManager& world);

    /// Отсчитать воздух: под водой убывает, на воздухе прибывает.
    void tickBreath(f32 dt);

    /// Задохнулся: бег не включается, пока выносливость не поднимется
    /// до SPRINT_RESUME.
    bool winded_ = false;

    /// Самая высокая точка текущего полёта. Урон считается от неё, а
    /// не от скорости: скорость упирается в maxFallSpeed, и падение
    /// с двадцати блоков не отличалось бы от падения с шестидесяти.
    f32 fallPeakY_ = 0.f;

    /// Отсчитать смерть и вернуть игрока, когда время вышло.
    void tickDeath(world::ChunkManager& world, f32 dt);

    /// Как часто оглядываемся на колодцы: раз в кадр перебирать
    /// девять супер-чанков незачем, деревня на месте.
    f32 wellTimer_ = 0.f;

    /// Логово спрашиваем по тому же таймеру и по той же причине:
    /// девять супер-чанков каждый кадр — это девять запросов высоты
    /// рельефа, а логово никуда не денется за полсекунды.
    f32 lairTimer_ = 0.f;

    /// Тиры на прошлом кадре. Иначе заметить СМЕНУ нечем: сама
    /// Reputation хранит только текущее значение.
    factions::ReputationTier prevRepTiers_[(u8)factions::FactionId::Count]{};
    bool repTiersKnown_ = false;

    ecs::Registry* reg_ = nullptr;
    ecs::Entity    entity_{};
    glm::vec3      aimDir_{0, 0, -1};
};

} // namespace player

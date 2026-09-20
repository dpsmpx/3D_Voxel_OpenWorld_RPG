/**
 * @file npc_def.h
 * @brief NPC: роли, диалоги с ветвлением, поведение жителей.
 */
#pragma once
#include "../core/types.h"
#include "../factions/faction.h"
#include "../physics/creature_motion.h"
#include "../world/ai/pathfinding.h"
#include <vector>
#include <glm/glm.hpp>

namespace npc {

/// Роль NPC — определяет доступные взаимодействия.
enum class NpcRole : u8 {
    Villager = 0,   // простой житель, можно поговорить
    QuestGiver,     // выдаёт квесты
    Trader,         // торгует (Phase 12)
    Blacksmith,     // кузня (Phase 12)
    Guard,          // стражник, защищает деревню
    Healer,         // лечит игрока за золото (Phase 12)
    Courier,        // идёт по дороге из деревни в деревню
    Count
};

/// Определение типа NPC.
struct NpcDef {
    const char* name;
    NpcRole     role;
    factions::FactionId faction;

    f32 maxHealth;
    f32 moveSpeed;
    f32 attackDamage;      // для стражи
    f32 attackRange;
    f32 aggroRange;
    f32 bodyRadius;
    f32 bodyHeight;

    bool hostile;          // атакует ли игрока без провокации

    /// Оружие в руках, combat::WeaponId. Ноль — безоружный.
    ///
    /// Настоящее оружие из общего реестра, а не число урона в
    /// определении NPC: у стражи оттуда же берутся длина замаха,
    /// отбрасывание и скорость ударов, и правится это в одном месте
    /// на всю игру. Оснастка по нему же вешает клинок в правую руку.
    u16 weaponId;

    /// Бросается ли на угрозу деревне. Стража и кузнец — да, пекарь
    /// — нет: иначе «защитники» это вся деревня разом.
    bool defender;

    /// Внешний вид — параметры для воксельного рендера
    u32 bodyColor;
    u32 headColor;
    u32 accentColor;

    /// Диалоговые ключи (используются DialogueSystem)
    const char* dialogueRoot;
};

enum NpcTypeId : u16 {
    NPC_NONE = 0,
    NPC_VILLAGER,
    NPC_QUEST_GIVER,
    NPC_TRADER,
    NPC_BLACKSMITH,
    NPC_GUARD,
    NPC_HEALER,
    NPC_COURIER,
    NPC_COUNT
};

class NpcRegistry {
public:
    static const NpcRegistry& instance();
    const NpcDef& get(u16 id) const;

private:
    NpcRegistry();
    NpcDef defs_[NPC_COUNT];
};

inline const NpcRegistry& npcRegistry() { return NpcRegistry::instance(); }

/// Компонент ECS — NPC-сущность.
struct NpcTag {
    u16 id = NPC_NONE;

    /// Постоянный ключ особи: (super-chunk X, Z, номер в деревне).
    ///
    /// Житель не хранится в мире — он создаётся и удаляется по мере
    /// приближения игрока. Всё, что должно его пережить, опирается на
    /// этот ключ, а не на номер сущности: номер у каждого нового
    /// вселения свой. По нему же спавнер узнаёт убитых.
    u64 persistKey = 0;
};

/// FSM-состояние NPC
struct NpcAI {
    enum State : u8 {
        Idle = 0,
        Wander,
        Talk,
        Follow,
        Flee,
        Combat,
        Travel,     ///< посыльный идёт по дороге
        Dead,
    };

    State state       = Idle;
    u32   interactTarget = 0;    // игрок или другой NPC
    f32   stateTime   = 0.f;
    f32   wanderTimer = 0.f;
    glm::vec3 homePos{0};
    glm::vec3 wanderTarget{0};

    f32   attackCooldown = 0.f;
    f32   damageFlash = 0.f;
    f32   deathTimer = 0.f;

    /// Ноги. Считает physics::stepCreature — тем же кодом, что ходит
    /// игрок. Своей копии «коллизии» у NPC больше нет: та проверяла
    /// один столбец в центре, не знала ни ширины тела, ни потолка, ни
    /// ступеней, и житель входил в стену дома по плечи.
    physics::CreatureMotion motion{};
    f32   jumpCooldown = 0.f;

    /// Навигация. Защитник бежал к волку по прямой и утыкался в угол
    /// собственного дома; вокруг дома нужно обходить.
    std::vector<glm::ivec3> path;
    i32   pathIndex = 0;
    f32   repathCooldown = 0.f;
    world::ai::MoveParams moveParams{};

    /// Кого бьём. Не только у стражи: кузнец тоже защитник.
    u32   guardTarget = 0;
    /// Когда в последний раз искали угрозу.
    f32   scanCooldown = 0.f;
    /// Сколько ещё держаться настороже после боя, прежде чем
    /// возвращаться к своим делам.
    f32   alertTimer = 0.f;

    /// Только для Courier — куда он идёт по дороге.
    ///
    /// Посыльный ходит между двумя точками: дойдя до цели, меняет её
    /// местами с домом и идёт обратно. Одного поля хватает, потому
    /// что вторая точка — это homePos, который и так есть у каждого.
    glm::vec3 travelTarget{0};

    /// Только для QuestGiver — текущий предложенный квест
    u32   offeredQuest = 0;
    f32   offerCooldown = 0.f;

    /// Диалог (когда активен)
    bool  inDialogue = false;
};

} // namespace npc

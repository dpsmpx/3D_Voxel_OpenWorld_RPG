#pragma once
#include "../core/types.h"
#include "../factions/faction.h"
#include <glm/glm.hpp>

namespace npc {

// ============================================================
// Роль NPC — определяет доступные взаимодействия.
// ============================================================
enum class NpcRole : u8 {
    Villager = 0,   // простой житель, можно поговорить
    QuestGiver,     // выдаёт квесты
    Trader,         // торгует (Phase 12)
    Blacksmith,     // кузня (Phase 12)
    Guard,          // стражник, защищает деревню
    Healer,         // лечит игрока за золото (Phase 12)
    Count
};

// ============================================================
// Определение типа NPC.
// ============================================================
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

    // Внешний вид — параметры для воксельного рендера
    u32 bodyColor;
    u32 headColor;
    u32 accentColor;

    // Диалоговые ключи (используются DialogueSystem)
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

// ============================================================
// Компонент ECS — NPC-сущность.
// ============================================================
struct NpcTag {
    u16 id = NPC_NONE;
};

// FSM-состояние NPC
struct NpcAI {
    enum State : u8 {
        Idle = 0,
        Wander,
        Talk,
        Follow,
        Flee,
        Combat,
        Dead,
    };

    State state       = Idle;
    u32   interactTarget = 0;    // игрок или другой NPC
    f32   stateTime   = 0.f;
    f32   wanderTimer = 0.f;
    glm::vec3 homePos{0};
    glm::vec3 wanderTarget{0};

    f32   walkPhase = 0.f;
    f32   attackCooldown = 0.f;
    f32   damageFlash = 0.f;
    f32   deathTimer = 0.f;

    // Только для Guard
    u32   guardTarget = 0;

    // Только для QuestGiver — текущий предложенный квест
    u32   offeredQuest = 0;
    f32   offerCooldown = 0.f;

    // Диалог (когда активен)
    bool  inDialogue = false;
};

} // namespace npc

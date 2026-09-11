#pragma once
#include "../core/types.h"

namespace progression {

// ============================================================
// Древо Познания. Три ветки по 8 узлов в каждой.
// Узлы открываются очками навыков (1 за уровень).
// Максимальный ранг узла — 3 (кроме особых, у них 1).
// ============================================================
enum class SkillBranch : u8 {
    Strength = 0,   // Сила: урон, здоровье, отбрасывание, финишер
    Agility,        // Ловкость: скорость, крит, уклонение, комбо
    Wisdom,         // Мудрость: мана, магия, крафт, зачарование
    Count
};

enum class SkillNodeId : u16 {
    None = 0,

    // -------- Strength (Сила) --------
    Str_Toughness,       // +15 HP за ранг
    Str_HeavyHitter,     // +8% melee урона
    Str_Cleave,          // +12% радиус/конус атаки
    Str_Knockback,       // +30% сила отбрасывания
    Str_Berserker,       // +10% melee урона (всегда)
    Str_Execute,         // +12% melee урона (всегда, но флейворится как добивание)
    Str_ResonanceGain,   // +20% накопления резонанса
    Str_FinisherPower,   // +20% урон финишера

    // -------- Agility (Ловкость) --------
    Agi_Swiftness,       // +4% скорость движения
    Agi_Precision,       // +2.5% шанс крита
    Agi_QuickStrike,     // +6% скорость атаки
    Agi_Dodge,           // +4% шанс уклонения
    Agi_Acrobat,         // +15% высота прыжка
    Agi_ComboMaster,     // +5% скорость атаки (стек)
    Agi_DeadlyStrike,    // +20% множитель крита
    Agi_Shadowstep,      // +6% скорость движения (стек)

    // -------- Wisdom (Мудрость) --------
    Wis_ArcaneMind,      // +20 маны
    Wis_ManaFlow,        // +0.6 MP/сек регенерации
    Wis_SpellPower,      // +12% магический урон
    Wis_Enchanter,       // +20% эффект зачарований
    Wis_Alchemist,       // +20% эффект зелий
    Wis_CraftMaster,     // +1 к уровню крафта
    Wis_ArcaneShield,    // +5% магическое сопротивление
    Wis_ResonanceMaster, // +15% максимальный резонанс

    Count
};

constexpr u16 SKILL_NODE_COUNT = (u16)SkillNodeId::Count;

// ============================================================
// Описание узла — используется UI и логикой открытия.
// ============================================================
struct SkillNodeDef {
    SkillNodeId  id;
    SkillBranch  branch;
    const char*  name;
    const char*  description;
    u8           maxRank;      // 1..3
    u8           costPerRank;  // обычно 1
    SkillNodeId  parent;       // требуется ранг >= 1 у родителя
    u8           position;     // 0..7 — позиция в ветке (для UI)
};

// ============================================================
// Состояние дерева у конкретного персонажа.
// ============================================================
struct SkillTree {
    u8  ranks[SKILL_NODE_COUNT] = {};
    i32 unspentPoints      = 0;
    i32 totalPointsEarned  = 0;

    u8 rank(SkillNodeId id) const {
        u16 i = (u16)id;
        if (i >= SKILL_NODE_COUNT) return 0;
        return ranks[i];
    }

    // Может ли узел быть открыт (или повышен в ранге)?
    bool canUnlock(SkillNodeId id) const;

    // Открыть/повысить ранг. Возвращает true при успехе.
    bool unlock(SkillNodeId id);

    // Сколько очков вложено в ветку (для UI)
    i32 spentInBranch(SkillBranch b) const;

    // Сброс дерева (перераспределение, Phase 13)
    void reset();
};

// ============================================================
// Реестр определений. Singleton.
// ============================================================
class SkillRegistry {
public:
    static const SkillRegistry& instance();

    const SkillNodeDef& get(SkillNodeId id) const;
    const SkillNodeDef* nodesInBranch(SkillBranch b, u16& countOut) const;

private:
    SkillRegistry();
    SkillNodeDef defs_[SKILL_NODE_COUNT];
};

inline const SkillRegistry& skillTree() { return SkillRegistry::instance(); }

} // namespace progression
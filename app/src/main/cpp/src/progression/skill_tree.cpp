#include "skill_tree.h"
#include "../core/log.h"
#include <cstring>
#include <vector>

namespace progression {

namespace {
// Статический массив для nodesInBranch — ветки хранят ссылки на свои узлы.
struct BranchNodes {
    std::vector<SkillNodeDef> nodes;
};
BranchNodes gBranches[(u32)SkillBranch::Count];
} // namespace

SkillRegistry::SkillRegistry() {
    auto reg = [&](SkillNodeId id, SkillBranch b,
                   const char* name, const char* desc,
                   u8 maxRank, u8 cost,
                   SkillNodeId parent, u8 pos)
    {
        SkillNodeDef d{};
        d.id = id;
        d.branch = b;
        d.name = name;
        d.description = desc;
        d.maxRank = maxRank;
        d.costPerRank = cost;
        d.parent = parent;
        d.position = pos;
        defs_[(u16)id] = d;
        gBranches[(u32)b].nodes.push_back(d);
    };

    // -------- Strength --------
    reg(SkillNodeId::Str_Toughness,     SkillBranch::Strength,
        "Toughness",     "+15 max HP per rank.",
        3, 1, SkillNodeId::None, 0);
    reg(SkillNodeId::Str_HeavyHitter,   SkillBranch::Strength,
        "Heavy Hitter",  "+8% melee damage per rank.",
        3, 1, SkillNodeId::Str_Toughness, 1);
    reg(SkillNodeId::Str_Cleave,        SkillBranch::Strength,
        "Cleave",        "+12% attack range per rank.",
        3, 1, SkillNodeId::Str_HeavyHitter, 2);
    reg(SkillNodeId::Str_Knockback,     SkillBranch::Strength,
        "Impact",        "+30% knockback per rank.",
        2, 1, SkillNodeId::Str_Cleave, 3);
    reg(SkillNodeId::Str_Berserker,     SkillBranch::Strength,
        "Berserker",     "+10% melee damage per rank.",
        3, 1, SkillNodeId::Str_Knockback, 4);
    reg(SkillNodeId::Str_Execute,       SkillBranch::Strength,
        "Executioner",   "+12% melee damage per rank.",
        2, 1, SkillNodeId::Str_Berserker, 5);
    reg(SkillNodeId::Str_ResonanceGain, SkillBranch::Strength,
        "Resonant Strike","+20% resonance gain per rank.",
        3, 1, SkillNodeId::Str_Execute, 6);
    reg(SkillNodeId::Str_FinisherPower, SkillBranch::Strength,
        "Finisher",      "+20% finisher damage per rank.",
        3, 1, SkillNodeId::Str_ResonanceGain, 7);

    // -------- Agility --------
    reg(SkillNodeId::Agi_Swiftness,     SkillBranch::Agility,
        "Swiftness",     "+4% movement speed per rank.",
        3, 1, SkillNodeId::None, 0);
    reg(SkillNodeId::Agi_Precision,     SkillBranch::Agility,
        "Precision",     "+2.5% crit chance per rank.",
        3, 1, SkillNodeId::Agi_Swiftness, 1);
    reg(SkillNodeId::Agi_QuickStrike,   SkillBranch::Agility,
        "Quick Strike",  "+6% attack speed per rank.",
        3, 1, SkillNodeId::Agi_Precision, 2);
    reg(SkillNodeId::Agi_Dodge,         SkillBranch::Agility,
        "Dodge",         "+4% dodge chance per rank.",
        3, 1, SkillNodeId::Agi_QuickStrike, 3);
    reg(SkillNodeId::Agi_Acrobat,       SkillBranch::Agility,
        "Acrobat",       "+15% jump height per rank.",
        2, 1, SkillNodeId::Agi_Dodge, 4);
    reg(SkillNodeId::Agi_ComboMaster,   SkillBranch::Agility,
        "Combo Master",  "+5% attack speed per rank.",
        3, 1, SkillNodeId::Agi_Acrobat, 5);
    reg(SkillNodeId::Agi_DeadlyStrike,  SkillBranch::Agility,
        "Deadly Strike", "+20% crit damage per rank.",
        3, 1, SkillNodeId::Agi_ComboMaster, 6);
    reg(SkillNodeId::Agi_Shadowstep,    SkillBranch::Agility,
        "Shadowstep",    "+6% movement speed per rank.",
        2, 1, SkillNodeId::Agi_DeadlyStrike, 7);

    // -------- Wisdom --------
    reg(SkillNodeId::Wis_ArcaneMind,     SkillBranch::Wisdom,
        "Arcane Mind",   "+20 max mana per rank.",
        3, 1, SkillNodeId::None, 0);
    reg(SkillNodeId::Wis_ManaFlow,       SkillBranch::Wisdom,
        "Mana Flow",     "+0.6 MP/s regen per rank.",
        3, 1, SkillNodeId::Wis_ArcaneMind, 1);
    reg(SkillNodeId::Wis_SpellPower,     SkillBranch::Wisdom,
        "Spell Power",   "+12% spell damage per rank.",
        3, 1, SkillNodeId::Wis_ManaFlow, 2);
    reg(SkillNodeId::Wis_Enchanter,      SkillBranch::Wisdom,
        "Enchanter",     "+20% enchant power per rank.",
        2, 1, SkillNodeId::Wis_SpellPower, 3);
    reg(SkillNodeId::Wis_Alchemist,      SkillBranch::Wisdom,
        "Alchemist",     "+20% potion effect per rank.",
        2, 1, SkillNodeId::Wis_Enchanter, 4);
    reg(SkillNodeId::Wis_CraftMaster,    SkillBranch::Wisdom,
        "Craft Master",  "+1 crafting tier.",
        2, 1, SkillNodeId::Wis_Alchemist, 5);
    reg(SkillNodeId::Wis_ArcaneShield,   SkillBranch::Wisdom,
        "Arcane Shield", "+5% magic resist per rank.",
        3, 1, SkillNodeId::Wis_CraftMaster, 6);
    reg(SkillNodeId::Wis_ResonanceMaster,SkillBranch::Wisdom,
        "Resonance Master","+15% max resonance per rank.",
        3, 1, SkillNodeId::Wis_ArcaneShield, 7);

    LOGI("SkillRegistry: зарегистрировано %u узлов", (unsigned)((u16)SkillNodeId::Count - 1));
}

const SkillRegistry& SkillRegistry::instance() {
    static SkillRegistry r;
    return r;
}

const SkillNodeDef& SkillRegistry::get(SkillNodeId id) const {
    u16 i = (u16)id;
    if (i >= SKILL_NODE_COUNT) return defs_[0];
    return defs_[i];
}

const SkillNodeDef* SkillRegistry::nodesInBranch(SkillBranch b, u16& countOut) const {
    const auto& v = gBranches[(u32)b].nodes;
    countOut = (u16)v.size();
    return v.data();
}

// ============================================================
// SkillTree methods
// ============================================================

bool SkillTree::canUnlock(SkillNodeId id) const {
    if (id == SkillNodeId::None) return false;
    u16 i = (u16)id;
    if (i >= SKILL_NODE_COUNT) return false;

    const auto& def = skillTree().get(id);

    // Уже максимум?
    if (ranks[i] >= def.maxRank) return false;

    // Есть ли очки?
    if (unspentPoints < (i32)def.costPerRank) return false;

    // Родитель удовлетворён?
    if (def.parent != SkillNodeId::None) {
        u16 p = (u16)def.parent;
        if (ranks[p] == 0) return false;
    }

    return true;
}

bool SkillTree::unlock(SkillNodeId id) {
    if (!canUnlock(id)) return false;
    u16 i = (u16)id;
    const auto& def = skillTree().get(id);
    ranks[i] += 1;
    unspentPoints -= (i32)def.costPerRank;
    return true;
}

i32 SkillTree::spentInBranch(SkillBranch b) const {
    i32 sum = 0;
    for (u16 i = 1; i < SKILL_NODE_COUNT; ++i) {
        const auto& def = skillTree().get((SkillNodeId)i);
        if (def.branch == b) {
            sum += (i32)ranks[i];
        }
    }
    return sum;
}

void SkillTree::reset() {
    std::memset(ranks, 0, sizeof(ranks));
    // Очки возвращаем в пул
    unspentPoints = totalPointsEarned;
}

} // namespace progression
#include "attributes.h"
#include <algorithm>
#include <cmath>

namespace progression {

namespace {

// Единый источник формул. Любая правка баланса — только здесь.
inline f32 calcMaxHealth(const ecs::Attributes& a, u32 lvl) {
    return 100.f + a.endurance * 5.f + (f32)(lvl - 1) * 3.f;
}

inline f32 calcMaxMana(const ecs::Attributes& a, u32 lvl) {
    return 80.f + a.intelligence * 4.f + (f32)(lvl - 1) * 2.f;
}

inline f32 calcMaxStamina(const ecs::Attributes& a, u32 lvl) {
    return 100.f + a.endurance * 2.f + a.agility * 1.f + (f32)(lvl - 1) * 1.f;
}

inline f32 calcHealthRegen(const ecs::Attributes& a) {
    return 0.8f + a.endurance * 0.05f;
}

inline f32 calcManaRegen(const ecs::Attributes& a) {
    return 1.5f + a.intelligence * 0.10f;
}

inline f32 calcStaminaRegen(const ecs::Attributes& a) {
    return 8.f + a.endurance * 0.20f + a.agility * 0.10f;
}

inline f32 calcMeleeDamageMult(const ecs::Attributes& a) {
    return 1.f + a.strength * 0.020f;
}

inline f32 calcMagicDamageMult(const ecs::Attributes& a) {
    return 1.f + a.intelligence * 0.025f;
}

inline f32 calcAttackSpeedMult(const ecs::Attributes& a) {
    return 1.f + a.agility * 0.012f;
}

inline f32 calcCritChanceBonus(const ecs::Attributes& a) {
    return a.agility * 0.004f;   // +0.4% за 1 AGI
}

inline f32 calcCritDamageBonus(const ecs::Attributes& a) {
    return a.strength * 0.008f;  // +0.8% за 1 STR
}

inline f32 calcPhysicalResist(const ecs::Attributes& a) {
    f32 r = a.endurance * 0.006f;   // 0.6% за 1 END
    return std::min(0.60f, r);
}

inline f32 calcMagicResist(const ecs::Attributes& a) {
    f32 r = a.intelligence * 0.004f + a.endurance * 0.002f;
    return std::min(0.60f, r);
}

inline f32 calcDodgeChance(const ecs::Attributes& a) {
    f32 r = a.agility * 0.003f;   // 0.3% за 1 AGI
    return std::min(0.50f, r);
}

inline f32 calcMoveSpeedMult(const ecs::Attributes& a) {
    f32 r = a.agility * 0.004f;
    return 1.f + std::min(0.40f, r);
}

inline f32 calcJumpHeightMult(const ecs::Attributes& a) {
    f32 r = a.agility * 0.005f;
    return 1.f + std::min(0.60f, r);
}

// Накопитель всех бонусов дерева. Проходим один раз.
struct TreeAccum {
    f32 meleeDamageBonus    = 0.f;   // аддитивный множитель, добавляется
    f32 magicDamageBonus    = 0.f;
    f32 attackSpeedBonus    = 0.f;
    f32 critChanceAdd       = 0.f;
    f32 critDamageAdd       = 0.f;
    f32 maxHealthAdd        = 0.f;
    f32 maxManaAdd          = 0.f;
    f32 maxStaminaAdd       = 0.f;
    f32 healthRegenAdd      = 0.f;
    f32 manaRegenAdd        = 0.f;
    f32 staminaRegenAdd     = 0.f;
    f32 moveSpeedAdd        = 0.f;
    f32 jumpHeightAdd       = 0.f;
    f32 dodgeAdd            = 0.f;
    f32 knockbackAdd        = 0.f;
    f32 rangeAdd            = 0.f;
    f32 resonanceGainAdd    = 0.f;
    f32 finisherDamageAdd   = 0.f;
    f32 maxResonanceAdd     = 0.f;
    f32 enchantPowerAdd     = 0.f;
    f32 potionPowerAdd      = 0.f;
    f32 physicalResistAdd   = 0.f;
    f32 magicResistAdd      = 0.f;
    i32 craftTierAdd        = 0;
};

void applyNode(TreeAccum& t, SkillNodeId id, u8 rank) {
    if (rank == 0) return;
    const f32 r = (f32)rank;

    switch (id) {
        // -------- Strength --------
        case SkillNodeId::Str_Toughness:     t.maxHealthAdd      += 15.f * r; break;
        case SkillNodeId::Str_HeavyHitter:   t.meleeDamageBonus  += 0.08f * r; break;
        case SkillNodeId::Str_Cleave:        t.rangeAdd          += 0.12f * r; break;
        case SkillNodeId::Str_Knockback:     t.knockbackAdd      += 0.30f * r; break;
        case SkillNodeId::Str_Berserker:     t.meleeDamageBonus  += 0.10f * r; break;
        case SkillNodeId::Str_Execute:       t.meleeDamageBonus  += 0.12f * r; break;
        case SkillNodeId::Str_ResonanceGain: t.resonanceGainAdd  += 0.20f * r; break;
        case SkillNodeId::Str_FinisherPower: t.finisherDamageAdd += 0.20f * r; break;

        // -------- Agility --------
        case SkillNodeId::Agi_Swiftness:     t.moveSpeedAdd      += 0.04f * r; break;
        case SkillNodeId::Agi_Precision:     t.critChanceAdd     += 0.025f * r; break;
        case SkillNodeId::Agi_QuickStrike:   t.attackSpeedBonus  += 0.06f * r; break;
        case SkillNodeId::Agi_Dodge:         t.dodgeAdd          += 0.04f * r; break;
        case SkillNodeId::Agi_Acrobat:       t.jumpHeightAdd     += 0.15f * r; break;
        case SkillNodeId::Agi_ComboMaster:   t.attackSpeedBonus  += 0.05f * r; break;
        case SkillNodeId::Agi_DeadlyStrike:  t.critDamageAdd     += 0.20f * r; break;
        case SkillNodeId::Agi_Shadowstep:    t.moveSpeedAdd      += 0.06f * r; break;

        // -------- Wisdom --------
        case SkillNodeId::Wis_ArcaneMind:     t.maxManaAdd       += 20.f * r; break;
        case SkillNodeId::Wis_ManaFlow:       t.manaRegenAdd     += 0.60f * r; break;
        case SkillNodeId::Wis_SpellPower:     t.magicDamageBonus += 0.12f * r; break;
        case SkillNodeId::Wis_Enchanter:      t.enchantPowerAdd  += 0.20f * r; break;
        case SkillNodeId::Wis_Alchemist:      t.potionPowerAdd   += 0.20f * r; break;
        case SkillNodeId::Wis_CraftMaster:    t.craftTierAdd     += 1 * (i32)r; break;
        case SkillNodeId::Wis_ArcaneShield:   t.magicResistAdd   += 0.05f * r; break;
        case SkillNodeId::Wis_ResonanceMaster:t.maxResonanceAdd  += 0.15f * r; break;

        default: break;
    }
}

} // namespace

DerivedStats computeDerived(const ecs::Attributes& attr,
                            const SkillTree& tree,
                            u32 level)
{
    DerivedStats d{};

    // --- Базовые значения от атрибутов и уровня ---
    d.maxHealth    = calcMaxHealth(attr, level);
    d.maxMana      = calcMaxMana(attr, level);
    d.maxStamina   = calcMaxStamina(attr, level);
    d.healthRegen  = calcHealthRegen(attr);
    d.manaRegen    = calcManaRegen(attr);
    d.staminaRegen = calcStaminaRegen(attr);

    d.meleeDamageMult = calcMeleeDamageMult(attr);
    d.magicDamageMult = calcMagicDamageMult(attr);
    d.attackSpeedMult = calcAttackSpeedMult(attr);
    d.critChanceBonus = calcCritChanceBonus(attr);
    d.critDamageBonus = calcCritDamageBonus(attr);

    d.damageResistPhysical = calcPhysicalResist(attr);
    d.damageResistMagic    = calcMagicResist(attr);
    d.dodgeChance          = calcDodgeChance(attr);

    d.moveSpeedMult  = calcMoveSpeedMult(attr);
    d.jumpHeightMult = calcJumpHeightMult(attr);

    // --- Аккумулируем бонусы дерева ---
    TreeAccum t{};
    for (u16 i = 1; i < (u16)SkillNodeId::Count; ++i) {
        u8 rank = tree.rank((SkillNodeId)i);
        if (rank == 0) continue;
        applyNode(t, (SkillNodeId)i, rank);
    }

    // --- Накладываем аддитивные бонусы ---
    d.maxHealth    += t.maxHealthAdd;
    d.maxMana      += t.maxManaAdd;
    d.maxStamina   += t.maxStaminaAdd;
    d.healthRegen  += t.healthRegenAdd;
    d.manaRegen    += t.manaRegenAdd;
    d.staminaRegen += t.staminaRegenAdd;

    d.meleeDamageMult += t.meleeDamageBonus;
    d.magicDamageMult += t.magicDamageBonus;
    d.attackSpeedMult += t.attackSpeedBonus;
    d.critChanceBonus += t.critChanceAdd;
    d.critDamageBonus += t.critDamageAdd;

    d.damageResistPhysical = std::min(0.70f, d.damageResistPhysical + t.physicalResistAdd);
    d.damageResistMagic    = std::min(0.70f, d.damageResistMagic    + t.magicResistAdd);
    d.dodgeChance          = std::min(0.60f, d.dodgeChance          + t.dodgeAdd);

    d.moveSpeedMult  += t.moveSpeedAdd;
    d.jumpHeightMult += t.jumpHeightAdd;

    d.knockbackMult      = 1.f + t.knockbackAdd;
    d.rangeMult          = 1.f + t.rangeAdd;
    d.resonanceGainMult  = 1.f + t.resonanceGainAdd;
    d.finisherDamageMult = 1.f + t.finisherDamageAdd;
    d.maxResonanceMult   = 1.f + t.maxResonanceAdd;
    d.enchantPowerMult   = 1.f + t.enchantPowerAdd;
    d.potionPowerMult    = 1.f + t.potionPowerAdd;
    d.craftTierBonus     = t.craftTierAdd;

    // Ограничения — никаких отрицательных или абсурдных значений
    if (d.attackSpeedMult < 0.3f) d.attackSpeedMult = 0.3f;
    if (d.moveSpeedMult   < 0.3f) d.moveSpeedMult   = 0.3f;

    return d;
}

i32 totalAttributePointsFromLevel(u32 level) {
    if (level <= 1) return 0;
    return (i32)(level - 1) * ATTR_POINTS_PER_LEVEL;
}

} // namespace progression

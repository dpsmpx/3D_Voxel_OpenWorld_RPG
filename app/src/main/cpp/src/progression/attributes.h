/**
 * @file attributes.h
 * @brief Прогрессия: опыт, уровни, атрибуты, Древо Познания.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/components.h"
#include "skill_tree.h"

namespace progression {

// ============================================================
// Атрибуты — четыре базовые характеристики персонажа.
// Прокачиваются очками, получаемыми за уровень.
// ============================================================
// Используем ecs::Attributes как основной контейнер (STR/AGI/INT/END).
// Здесь — только производные характеристики.

constexpr i32 ATTR_MIN = 1;
constexpr i32 ATTR_MAX = 99;

/// Сколько очков атрибутов даётся за уровень
constexpr i32 ATTR_POINTS_PER_LEVEL = 2;

/// Производные характеристики, пересчитываемые при любом изменении
/// атрибутов/дерева/уровня. Используются боевой системой, физикой
/// и регенерацией.
struct DerivedStats {
    /// Ресурсы
    f32 maxHealth       = 100.f;
    f32 maxMana         = 80.f;
    f32 maxStamina      = 100.f;

    f32 healthRegen     = 1.0f;   // HP/сек
    f32 manaRegen       = 2.0f;   // MP/сек
    f32 staminaRegen    = 10.0f;  // SP/сек

    /// Боевые множители
    f32 meleeDamageMult   = 1.0f;
    f32 magicDamageMult   = 1.0f;
    f32 attackSpeedMult   = 1.0f;
    f32 critChanceBonus   = 0.0f;   // прибавляется к оружию
    f32 critDamageBonus   = 0.0f;   // прибавляется к оружию

    /// Выживаемость
    f32 damageResistPhysical = 0.0f;   // 0..0.6
    f32 damageResistMagic    = 0.0f;   // 0..0.6
    f32 dodgeChance          = 0.0f;   // 0..0.5

    /// Движение
    f32 moveSpeedMult   = 1.0f;
    f32 jumpHeightMult  = 1.0f;

    /// Утилитарные бонусы
    f32 knockbackMult       = 1.0f;
    f32 rangeMult           = 1.0f;
    f32 resonanceGainMult   = 1.0f;
    f32 finisherDamageMult  = 1.0f;
    f32 maxResonanceMult    = 1.0f;
    f32 enchantPowerMult    = 1.0f;
    f32 potionPowerMult     = 1.0f;
    i32 craftTierBonus      = 0;

    /// Модификатор расхода
    f32 manaCostMult     = 1.0f;
    f32 staminaCostMult  = 1.0f;
};

/// Расчёт производных. Чистая функция от трёх входов.
DerivedStats computeDerived(const ecs::Attributes& attr,
                            const SkillTree& tree,
                            u32 level);

/// Хелпер: сколько очков осталось распределить.
i32 totalAttributePointsFromLevel(u32 level);

} // namespace progression

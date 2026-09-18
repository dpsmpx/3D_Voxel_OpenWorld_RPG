/**
 * @file attributes.h
 * @brief Прогрессия: опыт, уровни, атрибуты, Древо Познания.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/components.h"
#include "skill_tree.h"
#include "../items/item_def.h"

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

/// Временные прибавки к атрибутам — от эликсиров.
///
/// По одной на атрибут: выпить два эликсира силы разом нельзя,
/// второй продлевает действие первого. Иначе стопка эликсиров
/// складывалась бы в произвольную силу, а таймеров пришлось бы
/// держать столько, сколько игрок успел выпить.
///
/// В сохранение не идут намеренно: минута действия не переживает
/// выход из игры, и формат сейва ради неё менять незачем.
struct AttributeBuffs {
    static constexpr u32 COUNT = 4;   ///< сила, ловкость, разум, стойкость
    i32 add[COUNT]{};
    f32 timeLeft[COUNT]{};

    /// Индекс атрибута, 0..3; COUNT — «никакой».
    static u32 indexOf(items::BuffAttr a) {
        switch (a) {
            case items::BuffAttr::Strength:     return 0;
            case items::BuffAttr::Agility:      return 1;
            case items::BuffAttr::Intelligence: return 2;
            case items::BuffAttr::Endurance:    return 3;
            default:                            return COUNT;
        }
    }

    /// Применить прибавку. Уже действующая не складывается, а
    /// обновляется: берётся большая прибавка и большее время.
    void apply(items::BuffAttr a, i32 amount, f32 seconds) {
        const u32 i = indexOf(a);
        if (i >= COUNT || amount == 0 || seconds <= 0.f) return;
        if (amount > add[i]) add[i] = amount;
        if (seconds > timeLeft[i]) timeLeft[i] = seconds;
    }

    /// Отсчитать время. Возвращает true, если что-то кончилось.
    bool tick(f32 dt) {
        bool expired = false;
        for (u32 i = 0; i < COUNT; ++i) {
            if (timeLeft[i] <= 0.f) continue;
            timeLeft[i] -= dt;
            if (timeLeft[i] > 0.f) continue;
            timeLeft[i] = 0.f;
            add[i] = 0;
            expired = true;
        }
        return expired;
    }

    bool any() const {
        for (u32 i = 0; i < COUNT; ++i) if (timeLeft[i] > 0.f) return true;
        return false;
    }
};

/// Атрибуты с учётом действующих эликсиров.
inline ecs::Attributes boosted(const ecs::Attributes& base,
                               const AttributeBuffs* b)
{
    ecs::Attributes out = base;
    if (!b) return out;
    out.strength     += b->add[0];
    out.agility      += b->add[1];
    out.intelligence += b->add[2];
    out.endurance    += b->add[3];
    return out;
}

/// Расчёт производных. Чистая функция от трёх входов.
DerivedStats computeDerived(const ecs::Attributes& attr,
                            const SkillTree& tree,
                            u32 level);

/// Хелпер: сколько очков осталось распределить.
i32 totalAttributePointsFromLevel(u32 level);

} // namespace progression

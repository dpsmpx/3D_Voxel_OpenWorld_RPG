#pragma once
#include "../core/types.h"

namespace combat {

// ============================================================
// Типы урона. Каждое оружие наносит один основной тип и может
// подмешивать вторичный (через зачарование).
// ============================================================
enum class DamageType : u8 {
    Physical = 0,
    Fire,
    Frost,
    Shock,
    Poison,
    Arcane,
    Count
};

// ============================================================
// Один "инстанс" урона. Всё, что нужно для расчёта попадания:
// сколько, какого типа, крит или нет, какие статусы наложить.
// ============================================================
struct DamageInstance {
    f32        amount       = 0.f;
    DamageType type         = DamageType::Physical;
    bool       isCritical   = false;
    f32        criticalMult = 1.5f;

    u32        sourceEntity = 0;
    u32        targetEntity = 0;

    // Статусы (влияют на цель после применения урона)
    f32 burnTime     = 0.f;   // Fire: цель горит N секунд
    f32 slowAmount   = 0.f;   // Frost: 0..1, замедление
    f32 slowDuration = 0.f;
    f32 stunDuration = 0.f;   // Shock: полная остановка
    f32 poisonDps    = 0.f;   // Poison: урон в секунду
    f32 poisonTime   = 0.f;

    // Для отладки/логов
    const char* sourceName = nullptr;
};

// ============================================================
// Сопротивление цели. Значения 0..0.9 (нельзя обнулить урон полностью).
// ============================================================
struct DamageResistance {
    f32 physical = 0.f;
    f32 fire     = 0.f;
    f32 frost    = 0.f;
    f32 shock    = 0.f;
    f32 poison   = 0.f;
    f32 arcane   = 0.f;

    f32 get(DamageType t) const {
        switch (t) {
            case DamageType::Physical: return physical;
            case DamageType::Fire:     return fire;
            case DamageType::Frost:    return frost;
            case DamageType::Shock:    return shock;
            case DamageType::Poison:   return poison;
            case DamageType::Arcane:   return arcane;
            default:                   return 0.f;
        }
    }
};

// ============================================================
// Финальный расчёт: amount * (1 + crit) * (1 - resistance).
// Клипуется в [0, ∞) — не даём отрицательного урона.
// ============================================================
f32 computeFinalDamage(const DamageInstance& dmg, const DamageResistance& res);

// ============================================================
// Единоразовый расчёт крита.
// ============================================================
bool rollCritical(f32 critChance);

} // namespace combat

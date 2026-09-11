#pragma once
#include "../core/types.h"
#include "damage.h"

namespace combat {

// ============================================================
// Типы зачарований. Накладываются на оружие в кузне (Phase 11)
// или выпадают как свитки/эссенции.
// ============================================================
enum class EnchantmentId : u8 {
    None = 0,
    Fire,        // +fire-урон и поджог
    Frost,       // +frost-урон и замедление
    Shock,       // +shock-урон и стан
    Poison,      // DoT-яд
    Vampiric,    // lifesteal
    Sharpness,   // +% базового урона
    Swift,       // +% скорости атаки
    Count
};

struct Enchantment {
    EnchantmentId id    = EnchantmentId::None;
    u8            level = 1;   // 1..3
};

// ============================================================
// Результат применения зачарования к одному удару.
// primary — модифицированный основной урон.
// secondary — вторичный магический урон (если зачарование
//             добавляет стихийный урон).
// lifestealFraction — доля нанесённого урона, возвращаемая в HP.
// attackSpeedMult — множитель скорости атаки (для Swift).
// ============================================================
struct EnchantResult {
    DamageInstance primary;
    DamageInstance secondary;
    f32 lifestealFraction = 0.f;
    f32 attackSpeedMult   = 1.f;
};

EnchantResult applyEnchantment(const DamageInstance& base,
                               const Enchantment& ench);

// Имя для UI
const char* enchantmentName(EnchantmentId id);

} // namespace combat

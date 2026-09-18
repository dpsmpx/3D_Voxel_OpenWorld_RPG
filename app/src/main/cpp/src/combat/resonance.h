/**
 * @file resonance.h
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#pragma once
#include "../core/types.h"
#include <algorithm>

namespace combat {

constexpr i32 RESONANCE_MAX_STACKS  = 5;
constexpr f32 RESONANCE_MAX         = 100.f;
constexpr f32 RESONANCE_PER_HIT     = 12.f;
constexpr f32 RESONANCE_PER_CRIT    = 20.f;
constexpr f32 RESONANCE_DECAY_RATE  = 45.f;
constexpr f32 RESONANCE_DECAY_DELAY = 3.0f;
constexpr f32 FINISHER_COOLDOWN     = 2.5f;

struct ResonanceBonuses {
    f32 damageMult;
    f32 speedMult;
    f32 critBonus;
    f32 rangeMult;
};

/// Бонусы за текущий стек резонанса.
/// @param stack число накопленных зарядов, 0..RESONANCE_MAX
/// @return таблица множителей урона, скорости и крита
const ResonanceBonuses& resonanceBonuses(i32 stack);

struct ResonanceState {
    f32  value              = 0.f;

    /// Личный потолок резонанса. Базовый — RESONANCE_MAX; узел
    /// «Resonance Master» поднимает его.
    ///
    /// Ступени при этом остаются на прежних АБСОЛЮТНЫХ отметках:
    /// пятая по-прежнему на 100. Иначе «+15% максимума» оказался бы
    /// не усилением, а ослаблением — вся польза резонанса в ступенях,
    /// и растянутая шкала означала бы, что до пятой надо дольше
    /// бить. Поднятый потолок даёт запас СВЕРХ пятой ступени: он
    /// утекает первым, и ступень держится дольше, а финишер,
    /// считающий множитель от заполнения, бьёт сильнее.
    f32  maxValue           = RESONANCE_MAX;

    i32  stack              = 0;
    f32  timeSinceHit       = 999.f;
    f32  finisherCooldown   = 0.f;
    bool finisherReady      = false;
    i32  comboCounter       = 0;
    f32  recentHitFlash     = 0.f;

    /// Phase 9: gainMult — множитель накопления резонанса.
    void onHit(bool wasCritical, f32 gainMult = 1.f);
    void onMiss();
    void update(f32 dt);

    /// Возвращает множитель урона финишера.
    f32 consumeFinisher();

    const ResonanceBonuses& bonuses() const { return resonanceBonuses(stack); }
    f32 fill() const { return value / std::max(1.f, maxValue); }

    f32 stackFill() const {
        const f32 perStack = RESONANCE_MAX / (f32)RESONANCE_MAX_STACKS;
        f32 inStack = value - (f32)stack * perStack;
        if (inStack < 0.f) inStack = 0.f;
        return inStack / perStack;
    }
};

} // namespace combat

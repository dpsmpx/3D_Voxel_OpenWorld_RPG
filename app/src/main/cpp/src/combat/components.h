/**
 * @file components.h
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "damage.h"
#include "weapon.h"
#include "enchantment.h"
#include "resonance.h"
#include <glm/glm.hpp>

namespace combat {

/// Combatant — добавляется всем, кто может получать урон.
struct Combatant {
    DamageResistance resistance{};
    f32 knockbackResist = 0.f;
    u32 faction         = 0;
    f32 radius          = 0.5f;
    f32 height          = 1.8f;
};

/// Экипированное оружие.
struct EquippedWeapon {
    u16         weaponId = 0;
    Enchantment enchant{};
};

/// Машина состояний атаки.
struct WeaponState {
    enum Phase : u8 {
        Idle = 0,
        Windup,
        Active,
        Recovery,
    };

    Phase       phase      = Idle;
    f32         phaseTime  = 0.f;

    /// Где рука на дуге удара: −1 отведена в замахе, 0 покой,
    /// +1 прошла сквозь цель. См. anim::AnimState::attack.
    ///
    /// Знаковое. Раньше было 0..1 и означало «рука вперёд»: на
    /// замахе она шла ВПЕРЁД, на возврате — назад, то есть удар
    /// проигрывался задом наперёд и не предупреждал ни о чём.
    f32         swingAnim  = 0.f;
    glm::vec3   swingDir{ 0.f, 0.f, 1.f };

    bool        inputConsumed = false;
};

/// Статусные эффекты.
struct StatusEffects {
    f32 burnTime    = 0.f;
    f32 burnDps     = 0.f;

    f32 slowTime    = 0.f;
    f32 slowAmount  = 0.f;

    f32 stunTime    = 0.f;

    f32 poisonTime  = 0.f;
    f32 poisonDps   = 0.f;

    f32 flashTimer  = 0.f;

    /// Phase 9: последний, кто нанёс урон — для награды XP
    /// при смерти от DoT (burn/poison).
    u32 lastAttacker = 0;

    bool stunned()   const { return stunTime > 0.f; }
    f32  speedMult() const {
        if (stunned()) return 0.f;
        f32 s = 1.f - slowAmount;
        return s < 0.f ? 0.f : s;
    }
};

/// Фракции.
struct Faction {
    static constexpr u32 Player  = 1;
    static constexpr u32 Hostile = 2;
    static constexpr u32 Passive = 3;
    static constexpr u32 NPC     = 4;

    static u32 of(ecs::Registry& reg, ecs::Entity e);
    static bool hostileTo(u32 a, u32 b) { return a != 0 && b != 0 && a != b; }
};

} // namespace combat

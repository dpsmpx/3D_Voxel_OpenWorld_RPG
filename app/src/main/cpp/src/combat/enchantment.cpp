/**
 * @file enchantment.cpp
 * @brief Бой: урон, оружие, зачарования, система «Резонанс», статусы.
 */
#include "enchantment.h"
#include "../config/localization.h"
#include <algorithm>

namespace combat {

EnchantResult applyEnchantment(const DamageInstance& base,
                               const Enchantment& ench)
{
    EnchantResult r{};
    r.primary   = base;
    r.secondary = base;   // копия, но amount будет 0 если не нужно
    r.secondary.amount = 0.f;

    if (ench.id == EnchantmentId::None) return r;

    const f32 L = (f32)ench.level;

    switch (ench.id) {
        case EnchantmentId::Sharpness:
            r.primary.amount *= (1.f + 0.15f * L);
            break;

        case EnchantmentId::Swift:
            r.attackSpeedMult = 1.f + 0.10f * L;
            break;

        case EnchantmentId::Fire: {
            r.secondary.amount  = base.amount * 0.25f * L;
            r.secondary.type    = DamageType::Fire;
            r.secondary.burnTime = 2.5f + 0.5f * L;
            break;
        }

        case EnchantmentId::Frost: {
            r.secondary.amount       = base.amount * 0.15f * L;
            r.secondary.type         = DamageType::Frost;
            r.secondary.slowAmount   = std::min(0.70f, 0.20f * L);
            r.secondary.slowDuration = 2.f + 0.5f * L;
            break;
        }

        case EnchantmentId::Shock: {
            r.secondary.amount      = base.amount * 0.20f * L;
            r.secondary.type        = DamageType::Shock;
            r.secondary.stunDuration = 0.20f * L;
            break;
        }

        case EnchantmentId::Poison: {
            r.secondary.amount     = 0.f;
            r.secondary.type       = DamageType::Poison;
            r.secondary.poisonDps  = 1.5f * L;
            r.secondary.poisonTime = 5.f;
            break;
        }

        case EnchantmentId::Vampiric: {
            r.lifestealFraction = 0.10f * L;
            break;
        }

        default:
            break;
    }

    return r;
}

const char* enchantmentName(EnchantmentId id) {
    switch (id) {
        case EnchantmentId::None:     return "-";
        case EnchantmentId::Fire:     return config::tr("Fire");
        case EnchantmentId::Frost:    return config::tr("Frost");
        case EnchantmentId::Shock:    return config::tr("Shock");
        case EnchantmentId::Poison:   return config::tr("Poison");
        case EnchantmentId::Vampiric: return config::tr("Vampiric");
        case EnchantmentId::Sharpness:return config::tr("Sharpness");
        case EnchantmentId::Swift:    return config::tr("Swift");
        default:                      return "?";
    }
}

} // namespace combat

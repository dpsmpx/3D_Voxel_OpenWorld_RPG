#include "damage.h"
#include <cmath>
#include <random>

namespace combat {

namespace {
std::mt19937& rng() {
    static std::mt19937 g(0xC0FFEE);
    return g;
}
}

f32 computeFinalDamage(const DamageInstance& dmg, const DamageResistance& res) {
    if (dmg.amount <= 0.f) return 0.f;

    f32 raw = dmg.amount;
    if (dmg.isCritical) raw *= dmg.criticalMult;

    f32 r = res.get(dmg.type);
    if (r < 0.f) r = 0.f;
    if (r > 0.9f) r = 0.9f;

    f32 final = raw * (1.f - r);
    if (final < 0.f) final = 0.f;
    return final;
}

bool rollCritical(f32 critChance) {
    if (critChance <= 0.f) return false;
    if (critChance >= 1.f) return true;
    std::uniform_real_distribution<f32> d(0.f, 1.f);
    return d(rng()) < critChance;
}

} // namespace combat

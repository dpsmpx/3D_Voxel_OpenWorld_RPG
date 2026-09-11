#include "resonance.h"
#include <algorithm>

namespace combat {

namespace {
ResonanceBonuses BONUSES[RESONANCE_MAX_STACKS + 1] = {
    {  1.00f,  1.00f, 0.00f, 1.00f },
    {  1.05f,  1.03f, 0.02f, 1.02f },
    {  1.10f,  1.06f, 0.04f, 1.04f },
    {  1.18f,  1.10f, 0.06f, 1.06f },
    {  1.28f,  1.15f, 0.09f, 1.09f },
    {  1.40f,  1.20f, 0.12f, 1.12f },
};
} // namespace

const ResonanceBonuses& resonanceBonuses(i32 stack) {
    if (stack < 0) stack = 0;
    if (stack > RESONANCE_MAX_STACKS) stack = RESONANCE_MAX_STACKS;
    return BONUSES[stack];
}

void ResonanceState::onHit(bool wasCritical, f32 gainMult) {
    if (gainMult < 0.f) gainMult = 0.f;

    const f32 base = wasCritical ? RESONANCE_PER_CRIT : RESONANCE_PER_HIT;
    const f32 gain = base * gainMult;

    value = std::min(RESONANCE_MAX, value + gain);
    timeSinceHit = 0.f;
    ++comboCounter;
    recentHitFlash = 0.15f;

    const f32 perStack = RESONANCE_MAX / (f32)RESONANCE_MAX_STACKS;
    i32 newStack = (i32)(value / perStack);
    if (newStack > RESONANCE_MAX_STACKS) newStack = RESONANCE_MAX_STACKS;
    stack = newStack;

    if (stack >= RESONANCE_MAX_STACKS && finisherCooldown <= 0.f) {
        finisherReady = true;
    }
}

void ResonanceState::onMiss() {
    comboCounter = 0;
}

void ResonanceState::update(f32 dt) {
    timeSinceHit += dt;
    if (finisherCooldown > 0.f) {
        finisherCooldown = std::max(0.f, finisherCooldown - dt);
    }
    if (recentHitFlash > 0.f) {
        recentHitFlash = std::max(0.f, recentHitFlash - dt);
    }

    if (timeSinceHit > RESONANCE_DECAY_DELAY) {
        value = std::max(0.f, value - RESONANCE_DECAY_RATE * dt);
        const f32 perStack = RESONANCE_MAX / (f32)RESONANCE_MAX_STACKS;
        i32 newStack = (i32)(value / perStack);
        if (newStack < stack) {
            stack = newStack;
            if (stack < RESONANCE_MAX_STACKS) finisherReady = false;
        }
    }

    if (stack >= RESONANCE_MAX_STACKS && finisherCooldown <= 0.f) {
        finisherReady = true;
    } else {
        finisherReady = false;
    }
}

f32 ResonanceState::consumeFinisher() {
    if (!finisherReady) return 0.f;

    f32 mult = 3.0f + 2.0f * (value / RESONANCE_MAX);

    value = 0.f;
    stack = 0;
    finisherReady = false;
    finisherCooldown = FINISHER_COOLDOWN;
    timeSinceHit = 999.f;
    comboCounter = 0;

    return mult;
}

} // namespace combat

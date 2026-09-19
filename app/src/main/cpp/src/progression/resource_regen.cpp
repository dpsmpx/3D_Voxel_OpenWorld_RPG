/**
 * @file resource_regen.cpp
 * @brief Прогрессия: опыт, уровни, атрибуты, Древо Познания.
 */
#include "resource_regen.h"
#include "progression.h"
#include "../ecs/components.h"
#include <algorithm>

namespace progression {

bool tryConsumeMana(ecs::Registry& reg, ecs::Entity e, f32 amount) {
    auto* m = reg.get<ecs::Mana>(e);
    if (!m) return false;
    if (amount <= 0.f) return true;
    if (m->current < amount) return false;
    m->current -= amount;
    return true;
}

bool tryConsumeStamina(ecs::Registry& reg, ecs::Entity e, f32 amount) {
    auto* s = reg.get<ecs::Stamina>(e);
    if (!s) return false;
    if (amount <= 0.f) return true;
    if (s->current < amount) return false;
    s->current -= amount;
    addFatigue(reg, e, amount * FATIGUE_PER_STAMINA);
    return true;
}

void consumeMana(ecs::Registry& reg, ecs::Entity e, f32 amount) {
    auto* m = reg.get<ecs::Mana>(e);
    if (!m) return;
    m->current = std::max(0.f, m->current - amount);
}

void consumeStamina(ecs::Registry& reg, ecs::Entity e, f32 amount) {
    auto* s = reg.get<ecs::Stamina>(e);
    if (!s) return;
    s->current = std::max(0.f, s->current - amount);
    addFatigue(reg, e, amount * FATIGUE_PER_STAMINA);
}

void restoreMana(ecs::Registry& reg, ecs::Entity e, f32 amount) {
    auto* m = reg.get<ecs::Mana>(e);
    if (!m) return;
    m->current = std::min(m->max, m->current + amount);
}

void restoreStamina(ecs::Registry& reg, ecs::Entity e, f32 amount) {
    auto* s = reg.get<ecs::Stamina>(e);
    if (!s) return;
    // Упираемся в ПОТОЛОК, а не в max: зелье не отменяет усталость.
    // Иначе полоска заливалась бы поверх отрезанного утомлением
    // хвоста, и показанное расходилось бы с действительным.
    s->current = std::min(staminaCeiling(reg, e), s->current + amount);
}

void restoreHealth(ecs::Registry& reg, ecs::Entity e, f32 amount) {
    auto* h = reg.get<ecs::Health>(e);
    if (!h) return;
    h->current = std::min(h->max, h->current + amount);
}

void addFatigue(ecs::Registry& reg, ecs::Entity e, f32 amount) {
    auto* f = reg.get<ecs::Fatigue>(e);
    if (!f || amount <= 0.f) return;
    f->value = std::min(1.f, f->value + amount);
    f->restTimer = 0.f;
}

f32 staminaCeiling(ecs::Registry& reg, ecs::Entity e) {
    auto* s = reg.get<ecs::Stamina>(e);
    if (!s) return 0.f;
    auto* f = reg.get<ecs::Fatigue>(e);
    if (!f) return s->max;
    return s->max * (1.f - FATIGUE_CAP_LOSS * f->value);
}

f32 manaCostMult(ecs::Registry& reg, ecs::Entity e) {
    auto* prog = reg.get<Progression>(e);
    if (!prog) return 1.f;
    return prog->derived.manaCostMult;
}

f32 staminaCostMult(ecs::Registry& reg, ecs::Entity e) {
    auto* prog = reg.get<Progression>(e);
    if (!prog) return 1.f;
    return prog->derived.staminaCostMult;
}

} // namespace progression

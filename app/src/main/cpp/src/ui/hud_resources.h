#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../ecs/components.h"

namespace ui {

// ============================================================
// Актуальные ресурсы игрока для HUD. Читается из ECS один раз
// за кадр; в UI передаётся как обычная структура.
// ============================================================
struct HudResources {
    f32 hpCurrent = 0.f, hpMax = 1.f;
    f32 mpCurrent = 0.f, mpMax = 1.f;
    f32 spCurrent = 0.f, spMax = 1.f;

    f32 hpPct() const { return hpMax > 0.f ? hpCurrent / hpMax : 0.f; }
    f32 mpPct() const { return mpMax > 0.f ? mpCurrent / mpMax : 0.f; }
    f32 spPct() const { return spMax > 0.f ? spCurrent / spMax : 0.f; }

    bool hpLow() const { return hpPct() < 0.30f; }
    bool mpLow() const { return mpPct() < 0.20f; }
    bool spLow() const { return spPct() < 0.15f; }
};

// Читает Health/Mana/Stamina из registry. Если компонентов нет —
// возвращает значения по умолчанию (100/80/100).
inline HudResources readHudResources(ecs::Registry& reg, ecs::Entity e) {
    HudResources r{};

    if (auto* h = reg.get<ecs::Health>(e)) {
        r.hpCurrent = h->current;
        r.hpMax     = h->max > 0.f ? h->max : 1.f;
    }
    if (auto* m = reg.get<ecs::Mana>(e)) {
        r.mpCurrent = m->current;
        r.mpMax     = m->max > 0.f ? m->max : 1.f;
    }
    if (auto* s = reg.get<ecs::Stamina>(e)) {
        r.spCurrent = s->current;
        r.spMax     = s->max > 0.f ? s->max : 1.f;
    }

    return r;
}

} // namespace ui
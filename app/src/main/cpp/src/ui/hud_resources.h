/**
 * @file hud_resources.h
 * @brief Интерфейс: immediate-mode UI поверх Vulkan, HUD, меню.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../ecs/components.h"
#include "../progression/resource_regen.h"

namespace ui {

/// Актуальные ресурсы игрока для HUD. Читается из ECS один раз
/// за кадр; в UI передаётся как обычная структура.
struct HudResources {
    f32 hpCurrent = 0.f, hpMax = 1.f;
    f32 mpCurrent = 0.f, mpMax = 1.f;
    f32 spCurrent = 0.f, spMax = 1.f;
    /// Потолок выносливости, который опускает утомление, в долях от
    /// spMax. Единица — свеж. Без него полоска просто отказывалась бы
    /// наполняться доверху, и игрок читал бы это как поломку.
    f32 spCeilPct = 1.f;

    f32 hpPct() const { return hpMax > 0.f ? hpCurrent / hpMax : 0.f; }
    f32 mpPct() const { return mpMax > 0.f ? mpCurrent / mpMax : 0.f; }
    f32 spPct() const { return spMax > 0.f ? spCurrent / spMax : 0.f; }

    bool hpLow() const { return hpPct() < 0.30f; }
    bool mpLow() const { return mpPct() < 0.20f; }
    bool spLow() const { return spPct() < 0.15f; }
};

/// Читает Health/Mana/Stamina из registry. Если компонентов нет —
/// возвращает значения по умолчанию (100/80/100).
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
    if (auto* f = reg.get<ecs::Fatigue>(e))
        r.spCeilPct = 1.f - progression::FATIGUE_CAP_LOSS * f->value;

    return r;
}

} // namespace ui

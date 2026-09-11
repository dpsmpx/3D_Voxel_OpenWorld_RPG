#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../world/chunk_manager.h"
#include "damage.h"
#include "components.h"

namespace combat {

// ============================================================
// Наложить статусы из DamageInstance на StatusEffects.
// Если у цели нет StatusEffects — ничего не делает (создание
// компонента — это ответственность интеграции).
// ============================================================
void applyStatuses(StatusEffects& se, const DamageInstance& dmg);

// ============================================================
// Применить весь боевой пакет к цели: урон + статусы.
// Возвращает фактически нанесённый урон (после сопротивлений).
//
// Функция НЕ модифицирует Velocity/AIAgent — она только
// уменьшает Health и наполняет StatusEffects. Движение
// учитывает статусы в собственных апдейтах (Phase 15).
// ============================================================
f32 applyDamage(ecs::Registry& reg,
                ecs::Entity target,
                const DamageInstance& dmg);

// ============================================================
// Покадровое обновление статусов: DoT (burn, poison),
// уменьшение таймеров, визуальная вспышка.
// ============================================================
void tickStatuses(ecs::Registry& reg, f32 dt);

} // namespace combat
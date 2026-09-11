#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "attributes.h"

namespace progression {

// ============================================================
// Проверка и списание ресурсов. Возвращает true, если удалось
// списать — иначе атакующее действие блокируется.
// ============================================================
bool tryConsumeMana(ecs::Registry& reg, ecs::Entity e, f32 amount);
bool tryConsumeStamina(ecs::Registry& reg, ecs::Entity e, f32 amount);

// ============================================================
// Принудительное списание без проверки (для отладки и эффектов).
// ============================================================
void consumeMana(ecs::Registry& reg, ecs::Entity e, f32 amount);
void consumeStamina(ecs::Registry& reg, ecs::Entity e, f32 amount);

// ============================================================
// Мгновенное восстановление (для зелий в Phase 12).
// ============================================================
void restoreMana(ecs::Registry& reg, ecs::Entity e, f32 amount);
void restoreStamina(ecs::Registry& reg, ecs::Entity e, f32 amount);
void restoreHealth(ecs::Registry& reg, ecs::Entity e, f32 amount);

// ============================================================
// Хелперы для боевой системы — получить множители расхода
// с учётом производных характеристик.
// ============================================================
f32 manaCostMult(ecs::Registry& reg, ecs::Entity e);
f32 staminaCostMult(ecs::Registry& reg, ecs::Entity e);

} // namespace progression
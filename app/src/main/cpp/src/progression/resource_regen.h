/**
 * @file resource_regen.h
 * @brief Прогрессия: опыт, уровни, атрибуты, Древо Познания.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "attributes.h"

namespace progression {

/// Проверка и списание ресурсов. Возвращает true, если удалось
/// списать — иначе атакующее действие блокируется.
bool tryConsumeMana(ecs::Registry& reg, ecs::Entity e, f32 amount);
/// Списывает выносливость, если её хватает.
/// @return false и ничего не списано, если выносливости недостаточно
bool tryConsumeStamina(ecs::Registry& reg, ecs::Entity e, f32 amount);

/// Принудительное списание без проверки (для отладки и эффектов).
void consumeMana(ecs::Registry& reg, ecs::Entity e, f32 amount);
/// Списывает выносливость безусловно, обрезая по нулю.
/// Для действий, которые нельзя отменить (получение урона).
void consumeStamina(ecs::Registry& reg, ecs::Entity e, f32 amount);

/// Мгновенное восстановление (для зелий в Phase 12).
void restoreMana(ecs::Registry& reg, ecs::Entity e, f32 amount);
/// Восстанавливает выносливость, не превышая максимум.
void restoreStamina(ecs::Registry& reg, ecs::Entity e, f32 amount);
/// Восстанавливает здоровье, не превышая максимум.
void restoreHealth(ecs::Registry& reg, ecs::Entity e, f32 amount);

/// Хелперы для боевой системы — получить множители расхода
/// с учётом производных характеристик.
f32 manaCostMult(ecs::Registry& reg, ecs::Entity e);
/// Множитель расхода выносливости от атрибутов и навыков.
/// @return 1.0 без бонусов, меньше — расход снижен
f32 staminaCostMult(ecs::Registry& reg, ecs::Entity e);

} // namespace progression

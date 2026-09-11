/**
 * @file item_use.h
 * @brief Предметы: определения, инвентарь, лут, подбор, использование.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "item_stack.h"

namespace items {

/// Результат использования.
enum class UseResult : u8 {
    Ok = 0,
    NotUsable,
    NoEffect,
    Consumed,       // использован и потрачен
    Equipped,
    AlreadyEquipped,
    Failed,
};

/// Сообщение о результате применения предмета — для строки статуса.
const char* useResultString(UseResult r);

/// Использовать предмет из слота инвентаря.
///
///   - Potion: восстанавливает HP/MP/SP или даёт бафф (Phase 13)
///   - Food:   восстанавливает HP/SP
///   - Weapon: экипирует в combat::EquippedWeapon, старое оружие
///             возвращается в инвентарь
///   - Block:  не используется через UI (ставится в мир)
///   - Прочее: NotUsable
UseResult useItemFromSlot(ecs::Registry& reg,
                          ecs::Entity playerEntity,
                          u32 slotIndex);

/// Выбросить предмет из слота в мир (по направлению dir).
bool dropItemFromSlot(ecs::Registry& reg,
                      ecs::Entity playerEntity,
                      u32 slotIndex,
                      const glm::vec3& worldPos,
                      const glm::vec3& dir);

} // namespace items

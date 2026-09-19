/**
 * @file item_use.h
 * @brief Предметы: определения, инвентарь, лут, подбор, использование.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "item_stack.h"
#include "../world/chunk_manager.h"
#include <glm/glm.hpp>

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

/// Бросить метательный предмет из слота.
///
/// Отдельно от useItemFromSlot: броску нужны мир и направление
/// взгляда, а тому — только реестр. Тащить мир во все применения
/// предметов ради одной категории значило бы связать съеденное
/// яблоко с генерацией чанков.
///
/// @return Consumed при успехе, NoEffect если бросать некуда
UseResult throwItemFromSlot(ecs::Registry& reg,
                            world::ChunkManager& world,
                            ecs::Entity playerEntity,
                            u32 slotIndex,
                            const glm::vec3& origin,
                            const glm::vec3& dir);

/// Выбросить предмет из слота в мир (по направлению dir).
bool dropItemFromSlot(ecs::Registry& reg,
                      ecs::Entity playerEntity,
                      u32 slotIndex,
                      const glm::vec3& worldPos,
                      const glm::vec3& dir);

} // namespace items

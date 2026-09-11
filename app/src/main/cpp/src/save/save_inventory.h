/**
 * @file save_inventory.h
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#pragma once
#include "../core/types.h"
#include "../ecs/registry.h"
#include "../items/inventory.h"
#include "../items/currency.h"
#include "../items/item_pickup.h"
#include "save_format.h"

namespace save {

/// Сериализация инвентаря, кошелька, экипировки.
/// Вызывается из save_player.cpp после основных компонентов.
void serializeInventory(ByteWriter& w, ecs::Registry& reg, ecs::Entity player);
/// Читает инвентарь игрока из сейва.
/// @return false, если данные повреждены; читатель помечается ошибкой
bool deserializeInventory(ByteReader& r, ecs::Registry& reg, ecs::Entity player);

/// Сериализация пикапов в мире (чтобы игрок не терял лут при выходе).
void serializePickups(ByteWriter& w, ecs::Registry& reg);
/// Читает лежащие в мире предметы.
/// @return false при повреждённых данных
bool deserializePickups(ByteReader& r, ecs::Registry& reg);

} // namespace save

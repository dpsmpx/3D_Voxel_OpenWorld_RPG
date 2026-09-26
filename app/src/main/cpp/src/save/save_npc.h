/**
 * @file save_npc.h
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#pragma once
#include "../core/types.h"
#include "../mobs/spawner.h"
#include "../npc/npc_spawner.h"
#include "../world/hazards.h"
#include "save_format.h"

namespace save {

/// Сохранение состояния NPC.
///
/// Позиция, здоровье и состояние FSM жителей не сохраняются: они
/// детерминированно вселяются заново, когда игрок подходит к деревне.
///
/// Сохранять нужно ровно одно — кого убили. Список постоянных ключей
/// убитых ведёт спавнер, он же по нему решает, кого не вселять.
///
/// Раньше этот список записывался одним ключом (u32 из homePos),
/// читался — и выбрасывался: «Пока просто читаем и игнорируем».
/// Убитые возвращались и после загрузки, и просто после прогулки.
void serializeNpcState(ByteWriter& w, const npc::NpcSpawner& spawner);
/// Читает список убитых и отдаёт его спавнеру.
/// @return false при повреждённых данных
bool deserializeNpcState(ByteReader& r, npc::NpcSpawner& spawner);

/// Вскрытые тайники.
///
/// Лежит рядом с убитыми NPC, потому что это то же самое: список
/// того, что в мире УЖЕ случилось и не должно случиться снова.
void serializeTreasures(ByteWriter& w, const hazards::TreasureKeeper& keeper);
bool deserializeTreasures(ByteReader& r, hazards::TreasureKeeper& keeper);

/// Побеждённые боссы — там же и по той же причине. Спавнер вправе
/// не передаваться (проверки, инструменты): тогда пишется пустой
/// список, а прочитанный выбрасывается.
void serializeBosses(ByteWriter& w, const mobs::Spawner* spawner);
bool deserializeBosses(ByteReader& r, mobs::Spawner* spawner);

} // namespace save

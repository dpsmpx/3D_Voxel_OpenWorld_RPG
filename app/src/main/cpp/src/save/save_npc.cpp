/**
 * @file save_npc.cpp
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#include "save_npc.h"
#include "../core/log.h"

#include <utility>
#include <vector>

namespace save {

/// Разумный потолок на чтении. Не ограничение игры, а защита от
/// битого файла: ключей ровно столько, скольких жителей убили.
static constexpr u32 MAX_DEAD_KEYS = 200000;

namespace {

/// Список ключей: число и сами ключи. Один формат на всё, что в мире
/// уже случилось: убитые жители, вскрытые тайники, побеждённые боссы.
void writeKeys(ByteWriter& w, const std::vector<u64>& keys) {
    w.varU32((u32)keys.size());
    for (u64 k : keys) w.writeU64(k);
}

bool readKeys(ByteReader& r, std::vector<u64>& keys) {
    u32 count = 0;
    if (!r.varU32v(count)) return false;
    if (count > MAX_DEAD_KEYS) return false;
    keys.clear();
    keys.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 k = 0;
        if (!r.u64v(k)) return false;
        keys.push_back(k);
    }
    return true;
}

} // namespace

void serializeNpcState(ByteWriter& w, const npc::NpcSpawner& spawner) {
    writeKeys(w, spawner.deadKeys());
}

bool deserializeNpcState(ByteReader& r, npc::NpcSpawner& spawner) {
    std::vector<u64> keys;
    if (!readKeys(r, keys)) return false;
    spawner.setDeadKeys(std::move(keys));
    return true;
}

void serializeTreasures(ByteWriter& w, const hazards::TreasureKeeper& keeper) {
    writeKeys(w, keeper.openedKeys());
}

bool deserializeTreasures(ByteReader& r, hazards::TreasureKeeper& keeper) {
    std::vector<u64> keys;
    if (!readKeys(r, keys)) return false;
    keeper.setOpenedKeys(std::move(keys));
    return true;
}

void serializeBosses(ByteWriter& w, const mobs::Spawner* spawner) {
    static const std::vector<u64> none;
    writeKeys(w, spawner ? spawner->defeatedBosses() : none);
}

bool deserializeBosses(ByteReader& r, mobs::Spawner* spawner) {
    std::vector<u64> keys;
    if (!readKeys(r, keys)) return false;
    if (spawner) spawner->setDefeatedBosses(std::move(keys));
    return true;
}

} // namespace save

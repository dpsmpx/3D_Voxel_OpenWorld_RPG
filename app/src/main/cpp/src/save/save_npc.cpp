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

void serializeNpcState(ByteWriter& w, const npc::NpcSpawner& spawner) {
    const std::vector<u64>& keys = spawner.deadKeys();
    w.varU32((u32)keys.size());
    for (u64 k : keys) w.writeU64(k);
}

bool deserializeNpcState(ByteReader& r, npc::NpcSpawner& spawner) {
    u32 count = 0;
    if (!r.varU32v(count)) return false;
    if (count > MAX_DEAD_KEYS) return false;

    std::vector<u64> keys;
    keys.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 k = 0;
        if (!r.u64v(k)) return false;
        keys.push_back(k);
    }
    spawner.setDeadKeys(std::move(keys));
    return true;
}

} // namespace save

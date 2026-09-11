/**
 * @file world_delta.h
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#pragma once
#include "../core/types.h"
#include "../world/chunk_manager.h"
#include "save_format.h"
#include <unordered_map>
#include <vector>
#include <mutex>

namespace save {

/// Одна модификация блока: линейный индекс в чанке + ID.
/// Индекс вычисляется как (ly * CHUNK_SIZE + lz) * CHUNK_SIZE + lx.
struct BlockMod {
    u32 index;
    u16 block;
};

/// Дельта одного чанка — все блоки, изменённые игроком.
struct ChunkDelta {
    world::ChunkCoord     coord;
    std::vector<BlockMod> mods;
};

/// WorldDeltaStore — потокобезопасный трекер изменений.
/// Записывает блоки при setVoxel, применяет при загрузке.
class WorldDeltaStore {
public:
    /// Записать изменение блока в мировых координатах.
    void recordBlock(i32 wx, i32 wy, i32 wz, u16 newId);

    /// Очистить дельту конкретного чанка.
    void clearChunk(world::ChunkCoord c);

    /// Полный сброс (при новой игре).
    void clearAll();

    /// Применить все сохранённые дельты к миру (после создания чанков).
    void applyAll(world::ChunkManager& world) const;

    /// ---- Сериализация ----
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

    /// Метрики
    usize totalMods()   const;
    usize chunkCount()  const;

private:
    mutable std::mutex mtx_;
    std::unordered_map<world::ChunkCoord, ChunkDelta, world::ChunkCoordHash> deltas_;
};

} // namespace save

/**
 * @file world_delta.h
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#pragma once
#include "../core/types.h"
#include "../world/chunk_manager.h"
#include "save_format.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace save {

/// Одна модификация блока: линейный индекс в чанке + ID.
/// Индекс вычисляется как (ly * CHUNK_SIZE + lz) * CHUNK_SIZE + lx.
struct BlockMod {
    u32 index;
    u16 block;
    /// Что стояло в клетке по генерации. Только в памяти: в файл не
    /// пишется, при загрузке восстанавливается из самого мира, когда
    /// чанк строится. UNKNOWN — ещё не известно.
    ///
    /// Нужен затем, чтобы правка, вернувшая клетку к исходному
    /// (сломал и поставил обратно, выкопал и засыпал), исчезала из
    /// сохранения, а не копилась в нём навсегда.
    u16 orig = world::UNKNOWN;
};

/// Дельта одного чанка — все блоки, изменённые игроком.
struct ChunkDelta {
    world::ChunkCoord     coord;
    std::vector<BlockMod> mods;
};

/// WorldDeltaStore — потокобезопасный трекер изменений.
///
/// Сохранение хранит ТОЛЬКО то, что игрок изменил. Посещённые, но не
/// тронутые чанки не пишутся никуда: они восстанавливаются из зерна.
/// Правки записываются при setVoxel и накладываются на чанк в момент
/// его генерации — и при загрузке сохранения, и когда чанк, ушедший
/// из памяти, строится заново.
class WorldDeltaStore {
public:
    /// Записать изменение блока в мировых координатах.
    ///
    /// oldId — что стояло в клетке до записи (UNKNOWN — неизвестно).
    /// Если клетку вернули к тому, что было по генерации, правка
    /// удаляется, а чанк без правок исчезает из хранилища.
    void recordBlock(i32 wx, i32 wy, i32 wz, u16 newId,
                     u16 oldId = world::UNKNOWN);

    /// Очистить дельту конкретного чанка.
    void clearChunk(world::ChunkCoord c);

    /// Полный сброс (при новой игре).
    void clearAll();

    /// Наложить правки на только что сгенерированный чанк. Зовётся
    /// из генерации под замком вокселей; заодно запоминает, что там
    /// стояло по генерации.
    void applyTo(world::ChunkCoord coord, world::Chunk& chunk);

    /// Подключить хранилище к миру: запись правок и наложение их на
    /// каждый генерируемый чанк.
    ///
    /// Мир держит не само хранилище, а общий с ним якорь: хранилище,
    /// разрушенное раньше мира, обнуляет его, и поздняя задача
    /// генерации просто не находит правок, а не лезет в освобождённую
    /// память. Порядок разрушения поэтому не важен.
    void attach(world::ChunkManager& world);

    WorldDeltaStore() = default;
    ~WorldDeltaStore();
    WorldDeltaStore(const WorldDeltaStore&) = delete;
    WorldDeltaStore& operator=(const WorldDeltaStore&) = delete;

    /// ---- Сериализация ----
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

    /// Метрики
    usize totalMods()   const;
    usize chunkCount()  const;

private:
    struct Anchor {
        std::mutex       m;
        WorldDeltaStore* store = nullptr;
    };
    std::shared_ptr<Anchor> anchor_;

    mutable std::mutex mtx_;
    std::unordered_map<world::ChunkCoord, ChunkDelta, world::ChunkCoordHash> deltas_;
};

} // namespace save
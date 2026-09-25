/**
 * @file world_delta.cpp
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#include "world_delta.h"
#include <shared_mutex>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <unordered_map>
#include "../core/log.h"
#include "../world/block.h"

namespace save {

using namespace world;

namespace {

// Хранение блочных индексов внутри чанка.
// Должно совпадать с world::chunkIndex() из chunk.h.
inline u32 linearIndex(i32 lx, i32 ly, i32 lz) {
    return (u32)((ly * CHUNK_SIZE + lz) * CHUNK_SIZE + lx);
}


} // namespace

void WorldDeltaStore::recordBlock(i32 wx, i32 wy, i32 wz, u16 newId, u16 oldId) {
    if (wy < 0 || wy >= CHUNK_SIZE_Y) return;

    i32 cx = wx >> 5;
    i32 cz = wz >> 5;
    i32 lx = wx - (cx << 5);
    i32 lz = wz - (cz << 5);

    u32 idx = linearIndex(lx, wy, lz);

    std::lock_guard lk(mtx_);
    const ChunkCoord key{cx, cz};
    auto it = deltas_.find(key);

    if (it != deltas_.end()) {
        auto& mods = it->second.mods;
        for (usize i = 0; i < mods.size(); ++i) {
            BlockMod& m = mods[i];
            if (m.index != idx) continue;
            m.block = newId;
            // Клетка вернулась к тому, что было по генерации: правки
            // больше нет, и в сохранении ей делать нечего.
            if (m.orig != UNKNOWN && m.orig == newId) {
                mods[i] = mods.back();
                mods.pop_back();
                if (mods.empty()) deltas_.erase(it);
            }
            return;
        }
    }

    // Первая правка клетки: до неё в клетке стояло сгенерированное.
    // Поставили то же самое — это не изменение.
    if (oldId != UNKNOWN && oldId == newId) return;

    auto& d = it != deltas_.end() ? it->second : deltas_[key];
    d.coord = key;
    d.mods.push_back({ idx, newId, oldId });
}

void WorldDeltaStore::clearChunk(world::ChunkCoord c) {
    std::lock_guard lk(mtx_);
    deltas_.erase(c);
}

void WorldDeltaStore::clearAll() {
    std::lock_guard lk(mtx_);
    deltas_.clear();
}

void WorldDeltaStore::applyTo(world::ChunkCoord coord, world::Chunk& chunk) {
    std::lock_guard lk(mtx_);
    auto it = deltas_.find(coord);
    if (it == deltas_.end()) return;

    auto& mods = it->second.mods;
    for (usize i = 0; i < mods.size();) {
        BlockMod& m = mods[i];
        u16& voxel = chunk.voxels[m.index];
        // Что здесь сгенерировал мир — теперь известно. Правка из
        // сохранения, совпавшая с ним, — не правка: например, мир
        // сохранили до того, как игрок вернул блок на место.
        if (m.orig == UNKNOWN) m.orig = voxel;
        if (m.block == m.orig) {
            mods[i] = mods.back();
            mods.pop_back();
            continue;
        }
        voxel = m.block;
        ++i;
    }
    if (mods.empty()) deltas_.erase(it);
}

WorldDeltaStore::~WorldDeltaStore() {
    if (!anchor_) return;
    std::lock_guard lk(anchor_->m);
    anchor_->store = nullptr;
}

void WorldDeltaStore::attach(world::ChunkManager& world) {
    if (!anchor_) {
        anchor_ = std::make_shared<Anchor>();
        anchor_->store = this;
    }
    const std::shared_ptr<Anchor> a = anchor_;
    world.setBlockModifyCallback([a](i32 wx, i32 wy, i32 wz, u16 newId, u16 oldId) {
        std::lock_guard lk(a->m);
        if (a->store) a->store->recordBlock(wx, wy, wz, newId, oldId);
    });
    world.setChunkDeltaSource([a](world::ChunkCoord c, world::Chunk& chunk) {
        std::lock_guard lk(a->m);
        if (a->store) a->store->applyTo(c, chunk);
    });
}

void WorldDeltaStore::write(ByteWriter& w) const {
    std::lock_guard lk(mtx_);

    w.varU32((u32)deltas_.size());

    // Пустых чанков в хранилище не бывает: правка, вернувшая клетку к
    // исходному, удаляет себя, а с последней правкой уходит и чанк.
    for (const auto& [coord, delta] : deltas_) {
        w.writeI32(coord.x);
        w.writeI32(coord.z);
        w.varU32((u32)delta.mods.size());

        for (const auto& m : delta.mods) {
            w.varU32(m.index);
            w.writeU16(m.block);
        }
    }
}

bool WorldDeltaStore::read(ByteReader& r) {
    std::unordered_map<world::ChunkCoord, ChunkDelta, world::ChunkCoordHash> parsed;

    u32 chunkCount = 0;
    if (!r.varU32v(chunkCount)) return false;
    if (chunkCount > 100000u) {
        LOGE("WorldDelta: слишком много чанков: %u", chunkCount);
        return false;
    }

    for (u32 i = 0; i < chunkCount; ++i) {
        ChunkDelta d;
        if (!r.i32v(d.coord.x)) return false;
        if (!r.i32v(d.coord.z)) return false;

        u32 modCount = 0;
        if (!r.varU32v(modCount)) return false;
        if (modCount > 200000u) {
            LOGE("WorldDelta: слишком много модификаций: %u", modCount);
            return false;
        }
        d.mods.reserve(modCount);

        for (u32 j = 0; j < modCount; ++j) {
            BlockMod m;
            if (!r.varU32v(m.index)) return false;
            if (m.index >= (u32)(CHUNK_SIZE_Y * CHUNK_SIZE * CHUNK_SIZE)) return false;
            if (!r.u16v(m.block))    return false;
            m.orig = UNKNOWN;
            d.mods.push_back(m);
        }
        if (!d.mods.empty()) parsed[d.coord] = std::move(d);
    }
    std::lock_guard lk(mtx_);
    deltas_.swap(parsed);
    return true;
}

usize WorldDeltaStore::totalMods() const {
    std::lock_guard lk(mtx_);
    usize total = 0;
    for (const auto& [_, d] : deltas_) total += d.mods.size();
    return total;
}

usize WorldDeltaStore::chunkCount() const {
    std::lock_guard lk(mtx_);
    return deltas_.size();
}

} // namespace save
/**
 * @file world_delta.cpp
 * @brief Сохранения: бинарный формат, сжатие, дельты мира, слоты.
 */
#include "world_delta.h"
#include <shared_mutex>
#include <atomic>
#include <mutex>
#include <utility>
#include "../core/log.h"
#include "../core/job_system.h"
#include "../world/block.h"
#include <chrono>
#include <thread>

namespace save {

using namespace world;

namespace {

// Хранение блочных индексов внутри чанка.
// Должно совпадать с world::chunkIndex() из chunk.h.
inline u32 linearIndex(i32 lx, i32 ly, i32 lz) {
    return (u32)((ly * CHUNK_SIZE + lz) * CHUNK_SIZE + lx);
}

inline void decodeIndex(u32 idx, i32& lx, i32& ly, i32& lz) {
    lx   = (i32)(idx % CHUNK_SIZE);
    u32 r = idx / CHUNK_SIZE;
    lz   = (i32)(r % CHUNK_SIZE);
    ly   = (i32)(r / CHUNK_SIZE);
}

} // namespace

void WorldDeltaStore::recordBlock(i32 wx, i32 wy, i32 wz, u16 newId) {
    if (wy < 0 || wy >= CHUNK_SIZE_Y) return;

    i32 cx = wx >> 5;
    i32 cz = wz >> 5;
    i32 lx = wx - (cx << 5);
    i32 lz = wz - (cz << 5);

    u32 idx = linearIndex(lx, wy, lz);

    std::lock_guard lk(mtx_);
    auto& d = deltas_[ChunkCoord{cx, cz}];
    d.coord = { cx, cz };

    for (auto& m : d.mods) {
        if (m.index == idx) {
            m.block = newId;
            return;
        }
    }
    d.mods.push_back({ idx, newId });
}

void WorldDeltaStore::clearChunk(world::ChunkCoord c) {
    std::lock_guard lk(mtx_);
    deltas_.erase(c);
}

void WorldDeltaStore::clearAll() {
    std::lock_guard lk(mtx_);
    deltas_.clear();
}

bool WorldDeltaStore::applyAll(world::ChunkManager& world) const {
    std::lock_guard lk(mtx_);
    for (const auto& [coord, delta] : deltas_) {
        auto c = world.getChunk(coord.x, coord.z);
        if (!c) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!c->generated.load(std::memory_order_acquire)) {
            if (!jobs::gJobs.running()) return false;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (c->removed.load(std::memory_order_acquire)) return false;
        {
            // Пишем весь набор изменений под одним замком: меширование
            // не увидит чанк наполовину применённым.
            std::unique_lock vlk(c->voxelMutex);
            for (const auto& m : delta.mods) {
                i32 lx, ly, lz;
                decodeIndex(m.index, lx, ly, lz);
                c->voxels[linearIndex(lx, ly, lz)] = m.block;
            }
        }
        c->version.fetch_add(1, std::memory_order_release);
        world.requeueMesh(c);
    }
    return true;
}

void WorldDeltaStore::write(ByteWriter& w) const {
    std::lock_guard lk(mtx_);

    w.varU32((u32)deltas_.size());

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
            d.mods.push_back(m);
        }
        parsed[d.coord] = std::move(d);
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
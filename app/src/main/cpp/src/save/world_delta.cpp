#include "world_delta.h"
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

void WorldDeltaStore::applyAll(world::ChunkManager& world) const {
    std::lock_guard lk(mtx_);

    for (const auto& [coord, delta] : deltas_) {
        Chunk* c = world.getChunk(coord.x, coord.z);
        if (!c) continue;

        for (const auto& m : delta.mods) {
            i32 lx, ly, lz;
            decodeIndex(m.index, lx, ly, lz);
            c->voxels[linearIndex(lx, ly, lz)] = m.block;
        }
        c->version.fetch_add(1, std::memory_order_release);
    }
}

void WorldDeltaStore::write(ByteWriter& w) const {
    std::lock_guard lk(mtx_);

    w.varU32((u32)deltas_.size());

    for (const auto& [coord, delta] : deltas_) {
        w.i32(coord.x);
        w.i32(coord.z);
        w.varU32((u32)delta.mods.size());

        for (const auto& m : delta.mods) {
            w.varU32(m.index);
            w.u16(m.block);
        }
    }
}

bool WorldDeltaStore::read(ByteReader& r) {
    std::lock_guard lk(mtx_);
    deltas_.clear();

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
            if (!r.u16v(m.block))    return false;
            d.mods.push_back(m);
        }
        deltas_[d.coord] = std::move(d);
    }
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
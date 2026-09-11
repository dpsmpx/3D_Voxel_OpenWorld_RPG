#include "chunk_manager.h"
#include "../core/log.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace world {

ChunkManager::ChunkManager(u64 seed, i32 viewDistance)
    : seed_(seed), viewDistance_(viewDistance), gen_(seed)
{}

ChunkManager::~ChunkManager() {
    while (meshesInFlight_.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

ChunkManager::JobCtx* ChunkManager::acquireCtx(ChunkManager* mgr,
                                               ChunkCoord c, u64 v)
{
    std::lock_guard lk(jobCtxPoolMtx_);
    for (auto& p : jobCtxPool_) {
        if (!p) continue;
        auto* ctx = p.get();
        if (ctx->mgr == nullptr) {
            ctx->mgr = mgr; ctx->coord = c; ctx->version = v;
            return ctx;
        }
    }
    jobCtxPool_.push_back(std::make_unique<JobCtx>());
    auto* ctx = jobCtxPool_.back().get();
    ctx->mgr = mgr; ctx->coord = c; ctx->version = v;
    return ctx;
}

void ChunkManager::releaseCtx(JobCtx* ctx) {
    ctx->mgr = nullptr;
}

Chunk* ChunkManager::getChunk(i32 cx, i32 cz) {
    ChunkCoord key{cx, cz};
    {
        std::shared_lock lk(chunksMtx_);
        auto it = chunks_.find(key);
        if (it != chunks_.end()) return it->second.get();
    }
    std::unique_lock lk(chunksMtx_);
    auto it = chunks_.find(key);
    if (it != chunks_.end()) return it->second.get();

    auto c = std::make_unique<Chunk>();
    c->coord = {cx, 0, cz};
    Chunk* raw = c.get();
    chunks_[key] = std::move(c);

    enqueueGenerate(key);
    return raw;
}

void ChunkManager::enqueueGenerate(ChunkCoord coord) {
    JobCtx* ctx = acquireCtx(this, coord, 0);
    jobs::gJobs.submit(&ChunkManager::jobGenerate, ctx);
}

void ChunkManager::enqueueMesh(ChunkCoord coord) {
    Chunk* c = nullptr;
    {
        std::shared_lock lk(chunksMtx_);
        auto it = chunks_.find(coord);
        if (it == chunks_.end()) return;
        c = it->second.get();
    }
    u64 v = c->version.load(std::memory_order_acquire);
    meshesInFlight_.fetch_add(1, std::memory_order_relaxed);
    JobCtx* ctx = acquireCtx(this, coord, v);
    jobs::gJobs.submit(&ChunkManager::jobMesh, ctx);
}

void ChunkManager::jobGenerate(void* data) {
    auto* ctx = (ChunkManager::JobCtx*)data;
    ChunkManager* mgr = ctx->mgr;
    ChunkCoord coord = ctx->coord;
    mgr->releaseCtx(ctx);

    Chunk* c = nullptr;
    {
        std::shared_lock lk(mgr->chunksMtx_);
        auto it = mgr->chunks_.find(coord);
        if (it == mgr->chunks_.end()) return;
        c = it->second.get();
    }

    const auto& terrain = mgr->gen_;
    const i32 baseX = coord.x * CHUNK_SIZE;
    const i32 baseZ = coord.z * CHUNK_SIZE;

    for (i32 x = 0; x < CHUNK_SIZE; ++x) {
        for (i32 z = 0; z < CHUNK_SIZE; ++z) {
            const i32 wx = baseX + x;
            const i32 wz = baseZ + z;
            const i32 surface = terrain.surfaceHeight(wx, wz);
            const auto clim = terrain.sampleClimate(wx, wz, surface);
            const BiomeDef& biome = terrain.field().def(clim.biome);

            for (i32 y = 0; y < CHUNK_SIZE_Y; ++y) {
                u16 id = AIR;
                if (y == 0) {
                    id = BEDROCK;
                } else if (y < surface - 4) {
                    id = biome.stoneBlock;
                } else if (y < surface - 1) {
                    id = biome.subsurfaceBlock;
                } else if (y < surface) {
                    id = biome.surfaceBlock;
                }
                c->voxels[chunkIndex(x, y, z)] = id;
            }
        }
    }

    FeatureContext fctx{ &terrain, mgr->seed_ };
    applyCaves(*c, fctx);
    applyOres(*c, fctx);
    applyLiquids(*c, fctx);
    applyStructures(*c, fctx);
    applyTrees(*c, fctx);

    c->generated.store(true, std::memory_order_release);
    c->version.fetch_add(1, std::memory_order_release);
    mgr->enqueueMesh(coord);
}

void ChunkManager::jobMesh(void* data) {
    auto* ctx = (ChunkManager::JobCtx*)data;
    ChunkManager* mgr = ctx->mgr;
    ChunkCoord coord = ctx->coord;
    u64 version = ctx->version;
    mgr->releaseCtx(ctx);

    Chunk* c = nullptr;
    {
        std::shared_lock lk(mgr->chunksMtx_);
        auto it = mgr->chunks_.find(coord);
        if (it == mgr->chunks_.end()) {
            mgr->meshesInFlight_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        c = it->second.get();
    }
    if (!c->generated.load(std::memory_order_acquire)) {
        mgr->meshesInFlight_.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    ChunkNeighbors nb = mgr->gatherNeighbors(coord.x, coord.z);

    for (u8 i = 0; i < 4; ++i) {
        std::vector<Quad> quads;
        quads.reserve(i == 0 ? 4096 : (i == 1 ? 1024 : 256));
        buildGreedyMesh(*c, nb, quads, (Lod)i);

        std::unique_lock lk(mgr->readyMtx_);
        c->meshes[i].quads = std::move(quads);
        c->meshes[i].revision = version;
        c->meshes[i].ready.store(true, std::memory_order_release);
    }
    {
        std::unique_lock lk(mgr->readyMtx_);
        mgr->meshesReady_.push_back(c);
    }
    mgr->meshesInFlight_.fetch_sub(1, std::memory_order_relaxed);
}

ChunkNeighbors ChunkManager::gatherNeighbors(i32 cx, i32 cz) {
    ChunkNeighbors nb;
    std::shared_lock lk(chunksMtx_);
    auto it = chunks_.find({cx - 1, cz}); if (it != chunks_.end()) nb.nx = it->second.get();
    it = chunks_.find({cx + 1, cz});      if (it != chunks_.end()) nb.px = it->second.get();
    it = chunks_.find({cx, cz - 1});      if (it != chunks_.end()) nb.nz = it->second.get();
    it = chunks_.find({cx, cz + 1});      if (it != chunks_.end()) nb.pz = it->second.get();
    return nb;
}

std::vector<Chunk*> ChunkManager::pollMeshesReady() {
    std::vector<Chunk*> out;
    std::lock_guard lk(readyMtx_);
    out.swap(meshesReady_);
    return out;
}

void ChunkManager::update(const glm::vec3& playerPos) {
    ++frameCounter_;
    const i32 pcx = (i32)std::floor(playerPos.x / CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(playerPos.z / CHUNK_SIZE);

    for (i32 dz = -viewDistance_; dz <= viewDistance_; ++dz) {
        for (i32 dx = -viewDistance_; dx <= viewDistance_; ++dx) {
            if (dx*dx + dz*dz > viewDistance_*viewDistance_) continue;
            ChunkCoord c{pcx + dx, pcz + dz};
            {
                std::shared_lock lk(chunksMtx_);
                if (chunks_.count(c)) {
                    std::lock_guard lk2(lruMtx_);
                    lastAccess_[c] = frameCounter_;
                    continue;
                }
            }
            getChunk(c.x, c.z);
        }
    }
}

std::vector<ChunkCoord> ChunkManager::collectUnloadCandidates(const glm::vec3& playerPos) {
    std::vector<ChunkCoord> out;
    const i32 pcx = (i32)std::floor(playerPos.x / CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(playerPos.z / CHUNK_SIZE);
    const i32 R = viewDistance_ + 2;

    std::shared_lock lk(chunksMtx_);
    for (auto& [coord, chunk] : chunks_) {
        i32 dx = coord.x - pcx;
        i32 dz = coord.z - pcz;
        if (dx*dx + dz*dz > R*R) out.push_back(coord);
    }
    return out;
}

void ChunkManager::removeChunks(const std::vector<ChunkCoord>& coords) {
    std::unique_lock lk(chunksMtx_);
    for (auto& c : coords) chunks_.erase(c);
    {
        std::lock_guard lk2(lruMtx_);
        for (auto& c : coords) lastAccess_.erase(c);
    }
}

void ChunkManager::setVoxel(i32 wx, i32 wy, i32 wz, u16 block) {
    if (wy < 0 || wy >= CHUNK_SIZE_Y) return;
    i32 cx = wx >> 5;
    i32 cz = wz >> 5;
    Chunk* c = getChunk(cx, cz);
    i32 lx = wx - (cx << 5);
    i32 lz = wz - (cz << 5);
    c->set(lx, wy, lz, block);

    // ---- Phase 11: перехват изменения ----
    {
        std::lock_guard lk(callbackMtx_);
        if (blockModifyCb_) {
            blockModifyCb_(wx, wy, wz, block);
        }
    }

    enqueueMesh({cx, cz});
    enqueueMesh({cx-1, cz});
    enqueueMesh({cx+1, cz});
    enqueueMesh({cx, cz-1});
    enqueueMesh({cx, cz+1});
}

u16 ChunkManager::getVoxel(i32 wx, i32 wy, i32 wz) const {
    if (wy < 0 || wy >= CHUNK_SIZE_Y) return AIR;
    i32 cx = wx >> 5, cz = wz >> 5;
    std::shared_lock lk(chunksMtx_);
    auto it = chunks_.find({cx, cz});
    if (it == chunks_.end()) return AIR;
    return it->second->at(wx - (cx<<5), wy, wz - (cz<<5));
}

usize ChunkManager::loadedChunks() const {
    std::shared_lock lk(chunksMtx_);
    return chunks_.size();
}

} // namespace world

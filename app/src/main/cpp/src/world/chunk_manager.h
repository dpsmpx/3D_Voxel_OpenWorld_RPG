#pragma once
#include "chunk.h"
#include "terrain.h"
#include "features.h"
#include "../core/job_system.h"
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <functional>

namespace world {

struct ChunkCoord {
    i32 x, z;
    bool operator==(const ChunkCoord& o) const { return x == o.x && z == o.z; }
};
struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& c) const noexcept {
        u64 h = (u64)(u32)c.x | ((u64)(u32)c.z << 32);
        return (size_t)(h * 0x9E3779B97F4A7C15ULL);
    }
};

// ============================================================
// Callback для перехвата модификаций блоков.
// Устанавливается SaveManager'ом для записи в WorldDeltaStore.
// ============================================================
using BlockModifyCallback = std::function<void(i32 wx, i32 wy, i32 wz, u16 newId)>;

class ChunkManager {
public:
    explicit ChunkManager(u64 seed, i32 viewDistance = 8);
    ~ChunkManager();

    Chunk* getChunk(i32 cx, i32 cz);
    void   update(const glm::vec3& playerPos);
    std::vector<Chunk*> pollMeshesReady();

    // ---- Управление вокселями ----
    void setVoxel(i32 wx, i32 wy, i32 wz, u16 block);
    u16  getVoxel(i32 wx, i32 wy, i32 wz);

    // ---- Перехват изменений блоков (Phase 11) ----
    void setBlockModifyCallback(BlockModifyCallback cb) {
        std::lock_guard lk(callbackMtx_);
        blockModifyCb_ = std::move(cb);
    }

    // ---- Выгрузка чанков ----
    std::vector<ChunkCoord> collectUnloadCandidates(const glm::vec3& playerPos);
    void removeChunks(const std::vector<ChunkCoord>& coords);

    void setViewDistance(i32 d) { viewDistance_ = d; }
    i32  viewDistance() const   { return viewDistance_; }
    u64  seed() const { return seed_; }

    const TerrainGenerator& generator() const { return gen_; }

    usize loadedChunks() const;
    usize pendingGeneration() const { return meshesInFlight_.load(); }

private:
    struct JobCtx {
        ChunkManager* mgr;
        ChunkCoord    coord;
        u64           version;
    };

    void enqueueGenerate(ChunkCoord coord);
    void enqueueMesh(ChunkCoord coord);
    ChunkNeighbors gatherNeighbors(i32 cx, i32 cz);

    static void jobGenerate(void* data);
    static void jobMesh(void* data);

    JobCtx* acquireCtx(ChunkManager* mgr, ChunkCoord c, u64 v);
    void    releaseCtx(JobCtx* ctx);

    u64 seed_;
    i32 viewDistance_;
    TerrainGenerator gen_;

    std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash> chunks_;
    mutable std::shared_mutex chunksMtx_;

    std::mutex                readyMtx_;
    std::vector<Chunk*>       meshesReady_;

    std::atomic<u32>          meshesInFlight_{0};

    std::unordered_map<ChunkCoord, u64, ChunkCoordHash> lastAccess_;
    u64                       frameCounter_ = 0;
    mutable std::mutex        lruMtx_;

    // Callback (Phase 11)
    BlockModifyCallback       blockModifyCb_;
    mutable std::mutex        callbackMtx_;

    std::vector<std::unique_ptr<JobCtx>> jobCtxPool_;
    std::mutex                          jobCtxPoolMtx_;
};

} // namespace world
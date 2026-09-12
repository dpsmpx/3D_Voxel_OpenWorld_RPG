/**
 * @file chunk_manager.h
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
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
#include <utility>
#include <vector>

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

/// Callback для перехвата модификаций блоков.
/// Устанавливается SaveManager'ом для записи в WorldDeltaStore.
using BlockModifyCallback = std::function<void(i32 wx, i32 wy, i32 wz, u16 newId)>;

/// Соседи чанка, удерживаемые владеющими ссылками: пока объект жив,
/// ни один из чанков не может быть выгружен из-под задачи меширования.
struct NeighborLease {
    std::shared_ptr<Chunk> nx, px, nz, pz;
    ChunkNeighbors view() const {
        ChunkNeighbors n;
        n.nx = nx.get(); n.px = px.get();
        n.nz = nz.get(); n.pz = pz.get();
        return n;
    }
};

class ChunkManager {
public:
    explicit ChunkManager(u64 seed, i32 viewDistance = 8);
    ~ChunkManager();

    /// Возвращает чанк, при необходимости ставя его в очередь генерации.
    /// Владеющий указатель: чанк переживёт выгрузку, пока держат ссылку.
    std::shared_ptr<Chunk> getChunk(i32 cx, i32 cz);

    /// Чанк только если он уже загружен; генерацию не запускает.
    std::shared_ptr<Chunk> findChunk(i32 cx, i32 cz) const;

    void update(const glm::vec3& playerPos);

    /// Забирает чанки, у которых появились новые меши для выгрузки на GPU.
    std::vector<std::shared_ptr<Chunk>> pollMeshesReady();

    /// ---- Управление вокселями ----
    void setVoxel(i32 wx, i32 wy, i32 wz, u16 block);
    u16  getVoxel(i32 wx, i32 wy, i32 wz) const;

    /// ---- Перехват изменений блоков (Phase 11) ----
    void setBlockModifyCallback(BlockModifyCallback cb) {
        std::lock_guard lk(callbackMtx_);
        blockModifyCb_ = std::move(cb);
    }

    /// ---- Выгрузка чанков ----
    std::vector<ChunkCoord> collectUnloadCandidates(const glm::vec3& playerPos);
    void removeChunks(const std::vector<ChunkCoord>& coords);

    void setViewDistance(i32 d) { viewDistance_ = d; }
    i32  viewDistance() const   { return viewDistance_; }
    u64  seed() const { return seed_; }

    const TerrainGenerator& generator() const { return gen_; }

    /// Границы уровней детализации в блоках, приходят из рендера.
    /// Миру они нужны ровно затем, чтобы только что сгенерированный
    /// чанк сразу мешировался под ту детализацию, с которой его
    /// увидят, а не строился сперва в полном разрешении и тут же
    /// перестраивался.
    void setLodBands(f32 lod0, f32 lod1, f32 lod2) {
        lodBand0_ = lod0; lodBand1_ = lod1; lodBand2_ = lod2;
    }
    u8 lodForChunk(ChunkCoord c) const;

    /// Просит перестроить чанк под нужный уровень детализации.
    /// Вызывает рендер, когда обнаружил, что готового меша для
    /// текущей дистанции нет. Работа уходит в фон.
    void requestLod(ChunkCoord coord, u8 lod);

    usize loadedChunks() const;

    /// Сгенерирован ли чанк, накрывающий точку. Пока нет, мир о ней
    /// ничего не знает: двигать в нём что-либо бессмысленно и вредно.
    bool isReadyAt(i32 wx, i32 wz) const;
    usize pendingJobs() const { return jobsInFlight_.load(std::memory_order_relaxed); }

private:
    /// Контекст задачи владеет чанком: задача не может застать его
    /// уничтоженным, даже если игрок ушёл и чанк выгружен.
    struct JobCtx {
        ChunkManager*          mgr = nullptr;
        std::shared_ptr<Chunk> chunk;
        ChunkCoord             coord{};
        u64                    version = 0;
    };

    void enqueueGenerate(ChunkCoord coord);
    void enqueueMesh(ChunkCoord coord);
    NeighborLease gatherNeighbors(i32 cx, i32 cz) const;

    static void jobGenerate(void* data);
    static void jobMesh(void* data);

    u64 seed_;
    i32 viewDistance_;
    f32 lodBand0_ = 64.f, lodBand1_ = 160.f, lodBand2_ = 320.f;
    /// Последняя известная позиция игрока — по ней задача генерации
    /// решает, какой уровень детализации строить.
    std::atomic<i32> playerChunkX_{0};
    std::atomic<i32> playerChunkZ_{0};
    TerrainGenerator gen_;

    std::unordered_map<ChunkCoord, std::shared_ptr<Chunk>, ChunkCoordHash> chunks_;
    mutable std::shared_mutex chunksMtx_;

    std::mutex                                readyMtx_;
    std::vector<std::shared_ptr<Chunk>>       meshesReady_;

    /// Задачи генерации и меширования вместе: деструктор ждёт их все,
    /// иначе воркер обратится к уничтоженному ChunkManager.
    std::atomic<u32>          jobsInFlight_{0};

    std::unordered_map<ChunkCoord, u64, ChunkCoordHash> lastAccess_;
    u64                       frameCounter_ = 0;
    mutable std::mutex        lruMtx_;

    /// Callback (Phase 11)
    BlockModifyCallback       blockModifyCb_;
    mutable std::mutex        callbackMtx_;

};

} // namespace world

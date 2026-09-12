/**
 * @file chunk_manager.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "chunk_manager.h"
#include "../core/log.h"
#include "../core/memory.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace world {

ChunkManager::ChunkManager(u64 seed, i32 viewDistance)
    : seed_(seed), viewDistance_(viewDistance), gen_(seed)
{}

ChunkManager::~ChunkManager() {
    // Задачи держат сырой указатель на менеджер, поэтому дожидаемся
    // их завершения. Чанки они держат через shared_ptr и переживут
    // очистку карты, но сам ChunkManager — нет.
    {
        std::unique_lock lk(chunksMtx_);
        for (auto& [_, c] : chunks_) c->removed.store(true, std::memory_order_release);
    }
    while (jobsInFlight_.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ============================================================
// Доступ к чанкам
// ============================================================

std::shared_ptr<Chunk> ChunkManager::findChunk(i32 cx, i32 cz) const {
    std::shared_lock lk(chunksMtx_);
    auto it = chunks_.find(ChunkCoord{cx, cz});
    return it != chunks_.end() ? it->second : nullptr;
}

std::shared_ptr<Chunk> ChunkManager::getChunk(i32 cx, i32 cz) {
    const ChunkCoord key{cx, cz};
    {
        std::shared_lock lk(chunksMtx_);
        auto it = chunks_.find(key);
        if (it != chunks_.end()) return it->second;
    }

    std::shared_ptr<Chunk> created;
    {
        std::unique_lock lk(chunksMtx_);
        auto it = chunks_.find(key);
        if (it != chunks_.end()) return it->second;   // успели создать параллельно

        created = std::make_shared<Chunk>();
        created->coord = { cx, 0, cz };
        chunks_[key] = created;
    }
    enqueueGenerate(key);
    return created;
}

// ============================================================
// Постановка задач
// ============================================================

void ChunkManager::enqueueGenerate(ChunkCoord coord) {
    auto chunk = findChunk(coord.x, coord.z);
    if (!chunk) return;

    auto* ctx = new JobCtx{ this, std::move(chunk), coord, 0 };
    jobsInFlight_.fetch_add(1, std::memory_order_acq_rel);
    jobs::gJobs.submit(&ChunkManager::jobGenerate, ctx);
}

void ChunkManager::enqueueMesh(ChunkCoord coord) {
    auto chunk = findChunk(coord.x, coord.z);
    if (!chunk) return;
    if (!chunk->generated.load(std::memory_order_acquire)) return;

    const u64 v = chunk->version.load(std::memory_order_acquire);
    auto* ctx = new JobCtx{ this, std::move(chunk), coord, v };
    jobsInFlight_.fetch_add(1, std::memory_order_acq_rel);
    jobs::gJobs.submit(&ChunkManager::jobMesh, ctx);
}

// ============================================================
// Задача: генерация вокселей
// ============================================================

void ChunkManager::jobGenerate(void* data) {
    std::unique_ptr<JobCtx> ctx(static_cast<JobCtx*>(data));
    ChunkManager* mgr = ctx->mgr;
    Chunk* c = ctx->chunk.get();

    if (c->removed.load(std::memory_order_acquire)) {
        mgr->jobsInFlight_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    const auto& terrain = mgr->gen_;
    const i32 baseX = ctx->coord.x * CHUNK_SIZE;
    const i32 baseZ = ctx->coord.z * CHUNK_SIZE;

    // Колонки считаются один раз и переиспользуются фичами: без этого
    // высота и климат пересчитывались по четыре раза на колонку.
    static thread_local std::vector<TerrainGenerator::Column> columns;
    columns.resize(CHUNK_SIZE * CHUNK_SIZE);
    for (i32 x = 0; x < CHUNK_SIZE; ++x)
        for (i32 z = 0; z < CHUNK_SIZE; ++z)
            columns[x * CHUNK_SIZE + z] = terrain.column(baseX + x, baseZ + z);

    {
        // Пишем весь массив вокселей разом: черновые состояния наружу
        // не видны, читатели ждут на shared-замке.
        std::unique_lock lk(c->voxelMutex);

        for (i32 x = 0; x < CHUNK_SIZE; ++x) {
            for (i32 z = 0; z < CHUNK_SIZE; ++z) {
                const auto& col = columns[x * CHUNK_SIZE + z];
                const i32 surface = col.surface;
                const BiomeDef& biome = terrain.field().def(col.climate.biome);

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

        // Фичи дописывают тот же массив, замок уже наш.
        FeatureContext fctx{ &terrain, mgr->seed_, columns.data() };
        applyCaves(*c, fctx);
        applyOres(*c, fctx);
        applyLiquids(*c, fctx);
        applyStructures(*c, fctx);
        applyTrees(*c, fctx);
    }

    c->version.fetch_add(1, std::memory_order_release);
    c->generated.store(true, std::memory_order_release);

    mgr->jobsInFlight_.fetch_sub(1, std::memory_order_acq_rel);

    // Соседи рисуют свои граничные грани по нашим вокселям — перестроим их.
    mgr->enqueueMesh(ctx->coord);
    mgr->enqueueMesh({ ctx->coord.x - 1, ctx->coord.z });
    mgr->enqueueMesh({ ctx->coord.x + 1, ctx->coord.z });
    mgr->enqueueMesh({ ctx->coord.x, ctx->coord.z - 1 });
    mgr->enqueueMesh({ ctx->coord.x, ctx->coord.z + 1 });
}

// ============================================================
// Задача: построение мешей
// ============================================================

void ChunkManager::jobMesh(void* data) {
    std::unique_ptr<JobCtx> ctx(static_cast<JobCtx*>(data));
    ChunkManager* mgr = ctx->mgr;
    Chunk* c = ctx->chunk.get();

    struct InFlight {
        std::atomic<u32>& n;
        ~InFlight() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{ mgr->jobsInFlight_ };

    if (c->removed.load(std::memory_order_acquire)) return;
    if (!c->generated.load(std::memory_order_acquire)) return;

    // Устаревшая задача: воксели успели измениться ещё раз, и уже
    // поставлена более свежая. Свежая версия победит и так, но лишний
    // проход меширования дорогой — выходим.
    if (c->version.load(std::memory_order_acquire) != ctx->version) return;

    const NeighborLease lease = mgr->gatherNeighbors(ctx->coord.x, ctx->coord.z);

    // Чтение вокселей своего чанка и соседей — под shared-замками.
    // Захват строго в порядке возрастания адреса, иначе две задачи
    // на соседних чанках могут встать в клинч.
    {
        std::array<const Chunk*, 5> locked{
            c, lease.nx.get(), lease.px.get(), lease.nz.get(), lease.pz.get()
        };
        std::sort(locked.begin(), locked.end());

        std::array<std::shared_lock<std::shared_mutex>, 5> guards;
        for (usize i = 0; i < locked.size(); ++i) {
            if (!locked[i]) continue;
            if (i > 0 && locked[i] == locked[i - 1]) continue;   // дублей нет, но на всякий случай
            guards[i] = std::shared_lock<std::shared_mutex>(locked[i]->voxelMutex);
        }

        const ChunkNeighbors nb = lease.view();

        // Временные квады живут в арене воркера: за кадр меширования
        // это десятки тысяч элементов, и отдавать их системному
        // аллокатору по одному переаллоцированию на чанк дорого.
        // Арена переиспользуется между чанками — см. ТЗ 3.2 о пулах
        // и std::pmr.
        static thread_local mem::Arena         meshArena{ 1u << 20 };
        static thread_local mem::ArenaResource meshRes{ meshArena };
        meshArena.reset();
        std::pmr::vector<Quad> quads{ &meshRes };

        for (u8 lod = 0; lod < 4; ++lod) {
            quads.clear();
            buildGreedyMesh(*c, nb, quads, (Lod)lod);

            std::lock_guard mlk(c->meshMutex);
            // Копия в обычный vector: меш переживает арену воркера.
            c->meshes[lod].quads.assign(quads.begin(), quads.end());
            c->meshes[lod].revision = ctx->version;
            c->meshes[lod].built    = true;
            c->meshes[lod].ready.store(true, std::memory_order_release);
        }
    }

    if (c->removed.load(std::memory_order_acquire)) return;

    std::lock_guard lk(mgr->readyMtx_);
    // Один и тот же чанк мог уже попасть в очередь — не дублируем.
    for (const auto& r : mgr->meshesReady_) if (r.get() == c) return;
    mgr->meshesReady_.push_back(ctx->chunk);
}

NeighborLease ChunkManager::gatherNeighbors(i32 cx, i32 cz) const {
    NeighborLease lease;
    std::shared_lock lk(chunksMtx_);
    auto find = [&](i32 x, i32 z) -> std::shared_ptr<Chunk> {
        auto it = chunks_.find(ChunkCoord{x, z});
        if (it == chunks_.end()) return nullptr;
        // Сгенерированный сосед — иначе его воксели ещё нули, и грань
        // на стыке получится дырявой.
        if (!it->second->generated.load(std::memory_order_acquire)) return nullptr;
        return it->second;
    };
    lease.nx = find(cx - 1, cz);
    lease.px = find(cx + 1, cz);
    lease.nz = find(cx, cz - 1);
    lease.pz = find(cx, cz + 1);
    return lease;
}

std::vector<std::shared_ptr<Chunk>> ChunkManager::pollMeshesReady() {
    std::vector<std::shared_ptr<Chunk>> out;
    std::lock_guard lk(readyMtx_);
    out.swap(meshesReady_);
    return out;
}

// ============================================================
// Потоковая загрузка вокруг игрока
// ============================================================

void ChunkManager::update(const glm::vec3& playerPos) {
    ++frameCounter_;
    const i32 pcx = (i32)std::floor(playerPos.x / (f32)CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(playerPos.z / (f32)CHUNK_SIZE);

    for (i32 dz = -viewDistance_; dz <= viewDistance_; ++dz) {
        for (i32 dx = -viewDistance_; dx <= viewDistance_; ++dx) {
            if (dx*dx + dz*dz > viewDistance_*viewDistance_) continue;
            const ChunkCoord c{ pcx + dx, pcz + dz };
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
    const i32 pcx = (i32)std::floor(playerPos.x / (f32)CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(playerPos.z / (f32)CHUNK_SIZE);
    const i32 R = viewDistance_ + 2;   // гистерезис, чтобы не мигали на границе

    std::shared_lock lk(chunksMtx_);
    for (auto& [coord, chunk] : chunks_) {
        const i32 dx = coord.x - pcx;
        const i32 dz = coord.z - pcz;
        if (dx*dx + dz*dz > R*R) out.push_back(coord);
    }
    return out;
}

void ChunkManager::removeChunks(const std::vector<ChunkCoord>& coords) {
    std::vector<std::shared_ptr<Chunk>> doomed;
    doomed.reserve(coords.size());
    {
        std::unique_lock lk(chunksMtx_);
        for (const auto& c : coords) {
            auto it = chunks_.find(c);
            if (it == chunks_.end()) continue;
            it->second->removed.store(true, std::memory_order_release);
            doomed.push_back(std::move(it->second));
            chunks_.erase(it);
        }
    }
    {
        std::lock_guard lk2(lruMtx_);
        for (const auto& c : coords) lastAccess_.erase(c);
    }
    {
        // Задача меширования могла успеть положить чанк в очередь выгрузки.
        std::lock_guard lk3(readyMtx_);
        meshesReady_.erase(
            std::remove_if(meshesReady_.begin(), meshesReady_.end(),
                           [](const std::shared_ptr<Chunk>& c) {
                               return c->removed.load(std::memory_order_acquire);
                           }),
            meshesReady_.end());
    }
    // doomed освобождается здесь: если задача ещё держит чанк,
    // память проживёт ровно до её завершения.
}

// ============================================================
// Изменение вокселей
// ============================================================

void ChunkManager::setVoxel(i32 wx, i32 wy, i32 wz, u16 block) {
    if (wy < 0 || wy >= CHUNK_SIZE_Y) return;

    const i32 cx = wx >> 5;
    const i32 cz = wz >> 5;
    auto c = getChunk(cx, cz);
    if (!c) return;

    c->set(wx - (cx << 5), wy, wz - (cz << 5), block);

    {
        std::lock_guard lk(callbackMtx_);
        if (blockModifyCb_) blockModifyCb_(wx, wy, wz, block);
    }

    // Перестраиваем свой чанк и соседей: изменение на границе меняет
    // видимость граней у соседа.
    enqueueMesh({cx, cz});
    const i32 lx = wx - (cx << 5);
    const i32 lz = wz - (cz << 5);
    if (lx == 0)              enqueueMesh({cx - 1, cz});
    if (lx == CHUNK_SIZE - 1) enqueueMesh({cx + 1, cz});
    if (lz == 0)              enqueueMesh({cx, cz - 1});
    if (lz == CHUNK_SIZE - 1) enqueueMesh({cx, cz + 1});
}

u16 ChunkManager::getVoxel(i32 wx, i32 wy, i32 wz) const {
    // Выше мира — воздух, это правда. А вот ниже мира воздуха нет: там
    // дно. Раньше и туда отвечали воздухом, и провалившийся сквозь пол
    // падал без конца — ни вернуться, ни на что-то опереться.
    if (wy >= CHUNK_SIZE_Y) return AIR;
    if (wy < 0) return BEDROCK;

    const i32 cx = wx >> 5, cz = wz >> 5;

    std::shared_ptr<Chunk> c;
    {
        std::shared_lock lk(chunksMtx_);
        auto it = chunks_.find(ChunkCoord{cx, cz});
        // Чанка ещё нет — что там, неизвестно. Отвечать воздухом нельзя:
        // всё, что опирается на мир, шагнёт в пустоту.
        if (it == chunks_.end()) return BEDROCK;
        c = it->second;
    }

    // Чанк попадает в карту сразу, а генерируется потом, в фоне. До
    // конца генерации его воксели — нули, то есть воздух. Именно из-за
    // этого игрок проваливался сквозь мир на старте: под ним ещё ничего
    // не было сгенерировано, а мир отвечал «здесь пусто».
    if (!c->generated.load(std::memory_order_acquire)) return BEDROCK;

    std::shared_lock vlk(c->voxelMutex);
    return c->at(wx - (cx << 5), wy, wz - (cz << 5));
}

bool ChunkManager::isReadyAt(i32 wx, i32 wz) const {
    const i32 cx = wx >> 5, cz = wz >> 5;
    std::shared_lock lk(chunksMtx_);
    auto it = chunks_.find(ChunkCoord{cx, cz});
    if (it == chunks_.end()) return false;
    return it->second->generated.load(std::memory_order_acquire);
}

usize ChunkManager::loadedChunks() const {
    std::shared_lock lk(chunksMtx_);
    return chunks_.size();
}

} // namespace world

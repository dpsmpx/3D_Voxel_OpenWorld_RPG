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
    // Ждать можно только пока планировщик жив: после его остановки
    // очередь никто не разберёт, счётчик не сдвинется, и ожидание
    // станет вечным. На Android это выглядит как зависшее при выходе
    // приложение, которое система убивает за неотвечающий поток —
    // и настоящая причина выхода теряется.
    //
    // Такой порядок (остановить планировщик, потом разрушить мир)
    // считается ошибкой вызывающего, поэтому о нём говорим вслух.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (jobsInFlight_.load(std::memory_order_acquire) > 0) {
        if (!jobs::gJobs.running()) {
            LOGE("ChunkManager разрушается при остановленном планировщике: "
                 "%u задач не выполнится никогда",
                 jobsInFlight_.load(std::memory_order_acquire));
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            LOGE("ChunkManager: фоновые задачи не завершились за 5 с, "
                 "осталось %u — продолжаем разрушение",
                 jobsInFlight_.load(std::memory_order_acquire));
            break;
        }
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

    // Счётчик снимаем последним — как и в jobMesh. Раньше он снимался
    // сразу после генерации вокселей, а дальше задача ещё дважды
    // обращалась к менеджеру: за уровнем детализации и за постановкой
    // мешей соседям. Если эта задача была последней, деструктор
    // менеджера в этот момент переставал ждать и уничтожал его — и
    // обращения уходили в освобождённую память.
    struct InFlight {
        std::atomic<u32>& n;
        ~InFlight() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{ mgr->jobsInFlight_ };

    if (c->removed.load(std::memory_order_acquire)) return;

    const auto& terrain = mgr->gen_;
    // Колонки считаются один раз и переиспользуются фичами: без этого
    // высота и климат пересчитывались по четыре раза на колонку.
    static thread_local std::vector<TerrainGenerator::Column> columns;
    computeChunkColumns(terrain, ctx->coord.x, ctx->coord.z, columns);

    {
        // Пишем весь массив вокселей разом: черновые состояния наружу
        // не видны, читатели ждут на shared-замке.
        std::unique_lock lk(c->voxelMutex);
        generateChunkVoxels(*c, terrain, columns.data(), mgr->seed_);
    }

    c->version.fetch_add(1, std::memory_order_release);
    c->generated.store(true, std::memory_order_release);

    // Соседи рисуют свои граничные грани по нашим вокселям — перестроим их.
    // Сразу выбираем детализацию по расстоянию: иначе дальний чанк
    // сперва мешируется в полном разрешении, а через кадр
    // перестраивается в грубом — двойная работа на каждом новом чанке
    // у края мира, а их там больше всего.
    c->lodWanted.store(mgr->lodForChunk(ctx->coord), std::memory_order_release);

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

        // Строим один уровень — тот, который нужен рендеру. Четыре
        // уровня разом стоили вчетверо больше работы и хранились
        // целиком, хотя чанк почти всегда рисуется одним.
        const u8 lod = c->lodWanted.load(std::memory_order_acquire) & 3;
        buildGreedyMesh(*c, nb, quads, (Lod)lod);

        std::lock_guard mlk(c->meshMutex);
        // Копия в обычный vector: меш переживает арену воркера.
        c->meshes[lod].quads.assign(quads.begin(), quads.end());
        c->meshes[lod].revision = ctx->version;
        c->meshes[lod].built    = true;
        c->meshes[lod].ready.store(true, std::memory_order_release);

        // Остальные уровни устарели вместе с вокселями: их данные
        // больше не годятся, и держать их незачем.
        for (u8 other = 0; other < 4; ++other) {
            if (other == lod) continue;
            c->meshes[other].built = false;
            c->meshes[other].ready.store(false, std::memory_order_release);
            std::vector<Quad>().swap(c->meshes[other].quads);
        }
    }

    if (c->removed.load(std::memory_order_acquire)) return;

    // Один и тот же чанк мог уже попасть в очередь — не дублируем.
    // Флаг на самом чанке вместо перебора очереди: очередь читают и
    // пишут несколько рабочих потоков разом, и перебор под общим
    // замком дорожал вместе с её длиной.
    if (c->queuedForUpload.exchange(true, std::memory_order_acq_rel)) return;

    std::lock_guard lk(mgr->readyMtx_);
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

std::vector<std::shared_ptr<Chunk>> ChunkManager::pollMeshesReady(usize maxCount) {
    std::vector<std::shared_ptr<Chunk>> out;
    {
        std::lock_guard lk(readyMtx_);
        if (maxCount == 0 || meshesReady_.size() <= maxCount) {
            out.swap(meshesReady_);
        } else {
            // Берём с начала — то, что готово дольше всех. Хвост
            // остаётся до следующего кадра.
            out.assign(meshesReady_.begin(), meshesReady_.begin() + (long)maxCount);
            meshesReady_.erase(meshesReady_.begin(),
                               meshesReady_.begin() + (long)maxCount);
        }
    }
    // Вне очереди — значит можно ставить снова.
    for (const auto& c : out)
        if (c) c->queuedForUpload.store(false, std::memory_order_release);
    return out;
}

// ============================================================
// Потоковая загрузка вокруг игрока
// ============================================================

u8 ChunkManager::lodForChunk(ChunkCoord c) const {
    const f32 dx = (f32)(c.x - playerChunkX_.load(std::memory_order_relaxed))
                 * (f32)CHUNK_SIZE;
    const f32 dz = (f32)(c.z - playerChunkZ_.load(std::memory_order_relaxed))
                 * (f32)CHUNK_SIZE;
    const f32 d2 = dx * dx + dz * dz;
    if (d2 < lodBand0_ * lodBand0_) return 0;
    if (d2 < lodBand1_ * lodBand1_) return 1;
    if (d2 < lodBand2_ * lodBand2_) return 2;
    return 3;
}

void ChunkManager::update(const glm::vec3& playerPos) {
    ++frameCounter_;
    const i32 pcx = (i32)std::floor(playerPos.x / (f32)CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(playerPos.z / (f32)CHUNK_SIZE);
    playerChunkX_.store(pcx, std::memory_order_relaxed);
    playerChunkZ_.store(pcz, std::memory_order_relaxed);

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
                               if (!c->removed.load(std::memory_order_acquire))
                                   return false;
                               // Очередь его больше не держит — флаг
                               // не должен утверждать обратное.
                               c->queuedForUpload.store(false,
                                                        std::memory_order_release);
                               return true;
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

// ============================================================
// Курсор чтения вокселей: см. VoxelReader в заголовке.
// ============================================================
void VoxelReader::reopen(i32 cx, i32 cz) {
    // Старый замок отпускаем первым: держать два чанка разом незачем,
    // а лишний удерживаемый замок откладывал бы правки блоков.
    lk_ = std::shared_lock<std::shared_mutex>{};
    chunk_.reset();
    cx_ = cx; cz_ = cz;
    valid_ = true;
    haveChunk_ = false;

    chunk_ = mgr_->findChunk(cx, cz);
    if (!chunk_) return;
    if (!chunk_->generated.load(std::memory_order_acquire)) { chunk_.reset(); return; }
    lk_ = std::shared_lock<std::shared_mutex>(chunk_->voxelMutex);
    haveChunk_ = true;
}

u16 VoxelReader::at(i32 wx, i32 wy, i32 wz) {
    // Те же правила краёв, что у ChunkManager::getVoxel: выше мира
    // воздух, ниже — дно, незагруженное считается твёрдым.
    if (wy >= CHUNK_SIZE_Y) return AIR;
    if (wy < 0) return BEDROCK;

    const i32 cx = wx >> 5, cz = wz >> 5;
    if (!valid_ || cx != cx_ || cz != cz_) reopen(cx, cz);
    if (!haveChunk_) return BEDROCK;

    const i32 lx = wx & 31, lz = wz & 31;
    return chunk_->voxels[chunkIndex(lx, wy, lz)];
}

bool VoxelReader::isSolid(i32 wx, i32 wy, i32 wz) {
    return blocks().isSolid(at(wx, wy, wz));
}

u16 ChunkManager::getVoxel(i32 wx, i32 wy, i32 wz) const {
    // Выше мира — воздух, это правда. А вот ниже мира воздуха нет: там
    // дно. Раньше и туда отвечали воздухом, и провалившийся сквозь пол
    // падал без конца — ни вернуться, ни на что-то опереться.
    if (wy >= CHUNK_SIZE_Y) return AIR;
    if (wy < 0) return BEDROCK;

    const i32 cx = wx >> 5, cz = wz >> 5;

    // Замок карты чанков держим до конца чтения. Раньше здесь
    // копировался shared_ptr — чтобы отпустить замок пораньше, — но
    // это два атомарных изменения счётчика на каждый воксель, а
    // поиском пути и физикой их запрашивают тысячами за кадр. Пока
    // замок наш, чанк из карты никуда не денется и без счётчика.
    //
    // Порядок замков безопасен: обратного — сперва voxelMutex, потом
    // chunksMtx_ — в коде нет нигде.
    std::shared_lock lk(chunksMtx_);
    auto it = chunks_.find(ChunkCoord{cx, cz});
    // Чанка ещё нет — что там, неизвестно. Отвечать воздухом нельзя:
    // всё, что опирается на мир, шагнёт в пустоту.
    if (it == chunks_.end()) return BEDROCK;
    const Chunk* c = it->second.get();

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

void ChunkManager::requestLod(ChunkCoord coord, u8 lod) {
    auto chunk = findChunk(coord.x, coord.z);
    if (!chunk) return;
    if (!chunk->generated.load(std::memory_order_acquire)) return;
    const u8 want = lod & 3;
    chunk->lodWanted.store(want, std::memory_order_release);
    {
        std::lock_guard lk(chunk->meshMutex);
        if (chunk->meshes[want].built) return;   // уже построен, ждёт выгрузки
    }
    enqueueMesh(coord);
}

usize ChunkManager::loadedChunks() const {
    std::shared_lock lk(chunksMtx_);
    return chunks_.size();
}

} // namespace world

/**
 * @file chunk_manager.cpp
 * @brief Мир: чанки, процедурная генерация, биомы, структуры, цикл суток.
 */
#include "chunk_manager.h"
#include "debug_scene.h"
#include "../config/settings.h"
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
    // ПЕРВЫМ делом объявляем себя умершими.
    //
    // Дальше мы всё равно попробуем дождаться задач, но дождаться
    // получается не всегда: планировщик могли остановить раньше нас,
    // а задачи могли не уложиться в отведённые пять секунд. На этих
    // двух путях деструктор идёт дальше, не дождавшись, — и раньше
    // оставлял в очереди задачи с сырым указателем на себя. Теперь
    // они увидят снятый флаг и выйдут, ничего не тронув.
    host_->alive.store(false, std::memory_order_release);

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
    while (host_->inFlight.load(std::memory_order_acquire) > 0) {
        if (!jobs::gJobs.running()) {
            // Уже не опасно — задачи выйдут по снятому флагу, — но
            // всё ещё означает, что мир разрушают после планировщика.
            LOGW("ChunkManager разрушается при остановленном планировщике: "
                 "%u задач не выполнится никогда",
                 host_->inFlight.load(std::memory_order_acquire));
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            LOGE("ChunkManager: фоновые задачи не завершились за 5 с, "
                 "осталось %u — продолжаем разрушение",
                 host_->inFlight.load(std::memory_order_acquire));
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

    auto* ctx = new JobCtx{ host_, this, std::move(chunk), coord, 0, 0 };
    host_->inFlight.fetch_add(1, std::memory_order_acq_rel);
    jobs::gJobs.submit(&ChunkManager::jobGenerate, ctx);
}

void ChunkManager::enqueueMesh(ChunkCoord coord) {
    auto chunk = findChunk(coord.x, coord.z);
    if (!chunk) return;
    if (!chunk->generated.load(std::memory_order_acquire)) return;

    const u64 v = chunk->version.load(std::memory_order_acquire);
    // Номер заказа уезжает в задаче. Если пока она считает, меш
    // закажут заново, её результат устареет, и записывать его будет
    // нельзя — иначе старая задача затрёт состояние нового запроса.
    const u64 seq = chunk->meshSeq.fetch_add(1, std::memory_order_acq_rel) + 1;

    auto* ctx = new JobCtx{ host_, this, std::move(chunk), coord, v, seq };
    host_->inFlight.fetch_add(1, std::memory_order_acq_rel);
    jobs::gJobs.submit(&ChunkManager::jobMesh, ctx);
}


// ============================================================
// Задача: генерация вокселей
// ============================================================

void ChunkManager::jobGenerate(void* data) {
    std::unique_ptr<JobCtx> ctx(static_cast<JobCtx*>(data));

    // Счётчик снимаем последним — как и в jobMesh. Раньше он снимался
    // сразу после генерации вокселей, а дальше задача ещё
    // обращалась к менеджеру — за постановкой мешей себе и
    // соседям. Если эта задача была последней, деструктор
    // менеджера в этот момент переставал ждать и уничтожал его — и
    // обращения уходили в освобождённую память.
    //
    // Счётчик лежит в JobHost, а не в менеджере: снимать его мы будем
    // уже после того, как менеджера может не быть.
    struct InFlight {
        std::atomic<u32>& n;
        ~InFlight() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{ ctx->host->inFlight };

    // Менеджера может уже не быть: задача пролежала в очереди дольше,
    // чем прожил мир. Трогать mgr до этой проверки нельзя.
    if (!ctx->host->alive.load(std::memory_order_acquire)) return;

    ChunkManager* mgr = ctx->mgr;
    Chunk* c = ctx->chunk.get();

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
        if (config::settingsConst().debugScene)
            buildMinimalScene(*c);
        else
            generateChunkVoxels(*c, terrain, columns.data(), mgr->seed_);

        // Высоты поверхности — из тех же колонок, что уже посчитаны.
        // Даром: колонки всё равно в руках, а миникарте и траве иначе
        // придётся пересчитывать шум по каждой точке. Индексация та
        // же, что у computeChunkColumns: x * CHUNK_SIZE + z.
        for (i32 x = 0; x < CHUNK_SIZE; ++x)
            for (i32 z = 0; z < CHUNK_SIZE; ++z)
                c->surfaceY[(usize)x * CHUNK_SIZE + z] =
                    (i16)columns[(usize)x * CHUNK_SIZE + z].surface;
    }

    c->version.fetch_add(1, std::memory_order_release);
    c->generated.store(true, std::memory_order_release);

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

    struct InFlight {
        std::atomic<u32>& n;
        ~InFlight() { n.fetch_sub(1, std::memory_order_acq_rel); }
    } guard{ ctx->host->inFlight };

    // Как и в jobGenerate: пока не убедились, что менеджер жив, его
    // указателя для нас не существует.
    if (!ctx->host->alive.load(std::memory_order_acquire)) return;

    ChunkManager* mgr = ctx->mgr;
    Chunk* c = ctx->chunk.get();

    if (c->removed.load(std::memory_order_acquire)) return;
    if (!c->generated.load(std::memory_order_acquire)) return;

    // Устаревшая задача: воксели успели измениться ещё раз, и уже
    // поставлена более свежая. Свежая версия победит и так, но лишний
    // проход меширования дорогой — выходим.
    if (c->version.load(std::memory_order_acquire) != ctx->version) return;

    // То же самое по заказам на меширование: пока задача ждала своей
    // очереди, меш заказали заново. Считать дважды незачем.
    if (c->meshSeq.load(std::memory_order_acquire) != ctx->meshSeq) return;

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

        buildGreedyMesh(*c, nb, quads);

        std::lock_guard mlk(c->meshMutex);

        // Последняя сверка перед записью — уже под замком меша.
        //
        // Пока мы считали, воксели могли измениться, а этот уровень —
        // быть заказан заново. И в том и в другом случае наш результат
        // относится к прошлому, и класть его нельзя: снаружи он
        // неотличим от свежего, а более новый запрос оказался бы
        // затёрт задачей, которая старше его.
        if (c->version.load(std::memory_order_acquire) != ctx->version) return;
        if (c->meshSeq.load(std::memory_order_acquire) != ctx->meshSeq) return;

        // Копия в обычный vector: меш переживает арену воркера.
        c->mesh.quads.assign(quads.begin(), quads.end());
        c->mesh.revision = ctx->version;
        c->mesh.built    = true;
        c->mesh.ready.store(true, std::memory_order_release);
    }

    if (c->removed.load(std::memory_order_acquire)) return;

    // Тот же чанк мог уже попасть в очередь — не дублируем. Флаг на
    // самом чанке вместо перебора очереди: очередь читают и пишут
    // несколько рабочих потоков разом, и перебор под общим замком
    // дорожал вместе с её длиной.
    if (c->queuedForUpload.exchange(true, std::memory_order_acq_rel)) return;

    std::lock_guard lk(mgr->readyMtx_);
    mgr->meshesReady_.push_back(MeshReady{ ctx->chunk });
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

std::vector<MeshReady> ChunkManager::pollMeshesReady(usize maxCount) {
    std::vector<MeshReady> out;
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
        // Отметку снимаем ПОД ТЕМ ЖЕ ЗАМКОМ, что и изъятие из очереди.
        //
        // Снаружи между «вынули» и «сняли отметку» открывалось окно:
        // задача меширования, добравшаяся до конца ровно в нём, видела
        // отметку ещё поставленной, считала чанк уже стоящим в очереди
        // и выходила молча. Меш при этом построен, но в очередь не
        // попал — и на экране оставалась дыра ровно до тех пор, пока
        // тот же уровень не закажут заново.
        for (const auto& m : out)
            if (m.chunk)
                m.chunk->queuedForUpload.store(false,
                                                          std::memory_order_release);
    }
    return out;
}

void ChunkManager::requeueMesh(const std::shared_ptr<Chunk>& chunk) {
    if (!chunk) return;
    if (chunk->removed.load(std::memory_order_acquire)) return;
    {
        // Квадов нет — выгружать нечего, и вернуть в очередь значит
        // отправить рендер за ними ещё раз, и ещё, и так каждый кадр.
        std::lock_guard lk(chunk->meshMutex);
        if (!chunk->mesh.built) return;
    }
    // Тот же отбой дубликатов, что и в jobMesh: пока меш ездил в
    // рендер и обратно, задача меширования могла поставить его
    // заново, и второй записи в очереди быть не должно.
    if (chunk->queuedForUpload.exchange(true, std::memory_order_acq_rel)) return;

    std::lock_guard lk(readyMtx_);
    meshesReady_.push_back(MeshReady{ chunk });
}

// ============================================================
// Потоковая загрузка вокруг игрока
// ============================================================

void ChunkManager::update(const glm::vec3& playerPos) {
    if (!cameraKnown_.load(std::memory_order_acquire)) {
        cameraX_.store(playerPos.x, std::memory_order_relaxed);
        cameraY_.store(playerPos.y, std::memory_order_relaxed);
        cameraZ_.store(playerPos.z, std::memory_order_relaxed);
    }
    const i32 pcx = (i32)std::floor(playerPos.x / (f32)CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(playerPos.z / (f32)CHUNK_SIZE);

    rebuildStreamOrder();

    // Чанки заводятся по бюджету на кадр и строго от ближнего к
    // дальнему.
    //
    // Раньше здесь заводились ВСЕ недостающие чанки круга разом, в
    // порядке растрового обхода квадрата. На старте и после любого
    // заметного перемещения это два десятка мегабайт свежей памяти в
    // одном кадре: чанк — четверть мегабайта вокселей, и каждую
    // страницу ядро обнуляет при первом касании. Замер на хосте:
    // двести чанков, созданных и удержанных, — шестнадцать с
    // половиной миллисекунд, и ровно столько же показывал провал
    // world.update в долгой сессии (см. tools/soak). На телефоне
    // память медленнее.
    //
    // Порядок важен не меньше бюджета: под бюджетом растровый обход
    // заводил бы сперва угол квадрата, а чанк под ногами игрока —
    // последним. Обход идёт по возрастанию расстояния, поэтому мир
    // нарастает вокруг игрока, а не от угла.
    u32 created = 0;
    for (const auto& off : streamOrder_) {
        const ChunkCoord c{ pcx + off.dx, pcz + off.dz };
        {
            std::shared_lock lk(chunksMtx_);
            if (chunks_.count(c)) continue;
        }
        if (created >= MAX_NEW_CHUNKS_PER_UPDATE) break;   // остальные — в следующем кадре
        getChunk(c.x, c.z);
        ++created;
    }
}

void ChunkManager::rebuildStreamOrder() {
    if (streamOrderFor_ == viewDistance_) return;
    streamOrderFor_ = viewDistance_;
    streamOrder_.clear();
    const i32 vd = viewDistance_;
    streamOrder_.reserve((usize)(2 * vd + 1) * (usize)(2 * vd + 1));
    for (i32 dz = -vd; dz <= vd; ++dz)
        for (i32 dx = -vd; dx <= vd; ++dx) {
            const i32 d2 = dx * dx + dz * dz;
            if (d2 > vd * vd) continue;
            streamOrder_.push_back({ dx, dz, d2 });
        }
    std::sort(streamOrder_.begin(), streamOrder_.end(),
              [](const StreamOffset& a, const StreamOffset& b) {
                  return a.distSq < b.distSq;
              });
}

std::vector<ChunkCoord> ChunkManager::collectUnloadCandidates(const glm::vec3& playerPos,
                                                             i32 radiusChunks) {
    std::vector<ChunkCoord> out;
    const i32 pcx = (i32)std::floor(playerPos.x / (f32)CHUNK_SIZE);
    const i32 pcz = (i32)std::floor(playerPos.z / (f32)CHUNK_SIZE);
    // Гистерезис, чтобы чанки не мигали на границе круга.
    const i32 R = radiusChunks >= 0 ? radiusChunks : viewDistance_ + 2;

    std::shared_lock lk(chunksMtx_);
    for (auto& [coord, chunk] : chunks_) {
        const i32 dx = coord.x - pcx;
        const i32 dz = coord.z - pcz;
        if (dx*dx + dz*dz > R*R) out.push_back(coord);
    }
    return out;
}

std::vector<ChunkCoord> ChunkManager::collectOverBudget(
    const glm::vec3& playerPos) const
{
    std::vector<ChunkCoord> out;
    const usize cap = chunkCapacity();

    // Кандидаты собираем вместе с расстоянием: выгонять надо самых
    // дальних, а не первых попавшихся в хэш-таблице.
    struct Far { ChunkCoord coord; i64 d2; };
    std::vector<Far> far;
    {
        std::shared_lock lk(chunksMtx_);
        if (chunks_.size() <= cap) return out;
        const i32 pcx = (i32)std::floor(playerPos.x / (f32)CHUNK_SIZE);
        const i32 pcz = (i32)std::floor(playerPos.z / (f32)CHUNK_SIZE);
        far.reserve(chunks_.size());
        for (const auto& [coord, chunk] : chunks_) {
            const i64 dx = coord.x - pcx;
            const i64 dz = coord.z - pcz;
            far.push_back({ coord, dx * dx + dz * dz });
        }
    }

    const usize excess = far.size() - cap;
    // Нужны только `excess` самых дальних — полная сортировка всей
    // карты ради них не нужна.
    std::nth_element(far.begin(), far.begin() + (long)excess, far.end(),
                     [](const Far& a, const Far& b) { return a.d2 > b.d2; });
    out.reserve(excess);
    for (usize i = 0; i < excess; ++i) out.push_back(far[i].coord);
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
        // Задача меширования могла успеть положить чанк в очередь выгрузки.
        std::lock_guard lk3(readyMtx_);
        meshesReady_.erase(
            std::remove_if(meshesReady_.begin(), meshesReady_.end(),
                           [](const MeshReady& m) {
                               if (!m.chunk) return true;
                               if (!m.chunk->removed.load(std::memory_order_acquire))
                                   return false;
                               // Очередь его больше не держит — флаг
                               // не должен утверждать обратное.
                               m.chunk->queuedForUpload.store(
                                   false, std::memory_order_release);
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
    // Каждый чанк перестраивается в СВОЁМ текущем уровне, и уровень
    // выбирается здесь, а не в воркере.
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

i32 VoxelReader::surfaceAt(i32 wx, i32 wz) {
    const i32 cx = wx >> 5, cz = wz >> 5;
    if (!valid_ || cx != cx_ || cz != cz_) reopen(cx, cz);
    // Чанка ещё нет — считаем как раньше. Это честный запасной путь,
    // а не приблизительный: генератор и есть источник этого числа.
    if (!haveChunk_) return mgr_->generator().surfaceHeight(wx, wz);
    const i32 lx = wx & 31, lz = wz & 31;
    return (i32)chunk_->surfaceY[(usize)lx * CHUNK_SIZE + lz];
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

usize ChunkManager::loadedChunks() const {
    std::shared_lock lk(chunksMtx_);
    return chunks_.size();
}

} // namespace world

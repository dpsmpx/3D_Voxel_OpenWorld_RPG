/**
 * @file chunk_renderer.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "chunk_renderer.h"
#include "../core/log.h"
#include <algorithm>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace render {

bool ChunkRenderer::init(VkDevice dev, VkPhysicalDevice phys) {
    dev_ = dev; phys_ = phys;
    staging_.init(dev, phys);
    scratchVerts_.reserve(65536);
    scratchIndices_.reserve(98304);
    scratchQuads_.reserve(16384);
    return true;
}

void ChunkRenderer::shutdown() {
    for (auto& [_, m] : meshes_)
        for (auto& g : m.lod)
            if (g.valid) { g.vb.destroy(); g.ib.destroy(); }
    meshes_.clear();
    lodRequests_.clear();
    // Здесь уже всё дождались: устройство простаивает.
    for (auto& r : retired_) {
        vkDestroyBuffer(r.h.dev, r.h.buf, nullptr);
        vkFreeMemory(r.h.dev, r.h.mem, nullptr);
    }
    retired_.clear();
    staging_.destroy();
}

void ChunkRenderer::retire(vk::Buffer& b) {
    auto h = b.release();
    if (h.buf != VK_NULL_HANDLE) retired_.push_back({ frameNo_, h });
}

void ChunkRenderer::collectRetired() {
    // Столько кадров GPU может держать в работе; плюс один про запас.
    const u64 keep = (u64)vk::Context::MAX_FRAMES + 1;
    usize w = 0;
    for (usize i = 0; i < retired_.size(); ++i) {
        if (frameNo_ < retired_[i].frame + keep) {
            retired_[w++] = retired_[i];
            continue;
        }
        vkDestroyBuffer(retired_[i].h.dev, retired_[i].h.buf, nullptr);
        vkFreeMemory(retired_[i].h.dev, retired_[i].h.mem, nullptr);
    }
    retired_.resize(w);
}

void ChunkRenderer::forgetChunk(world::ChunkCoord c) {
    auto it = meshes_.find(c);
    if (it == meshes_.end()) return;
    // Чанк выгружается посреди кадра, а его буферы могут быть заняты
    // в ещё не показанных кадрах: уничтожение откладываем.
    for (auto& g : it->second.lod)
        if (g.valid) { retire(g.vb); retire(g.ib); }
    meshes_.erase(it);

    lodRequests_.erase(
        std::remove_if(lodRequests_.begin(), lodRequests_.end(),
                       [&](const LodRequest& r) { return r.coord == c; }),
        lodRequests_.end());
}

// ============================================================
// Загрузка одного уровня детализации в открытый пакет передачи.
// ============================================================
bool ChunkRenderer::uploadLod(vk::Context& ctx, VkCommandBuffer cmd,
                              ChunkGpu& gpu, world::Chunk& chunk, u8 lod)
{
    auto& gm = gpu.lod[lod];

    // Вершины строим прямо из хранимых квадов, под замком меша.
    // Копия всего списка, которая была здесь раньше, ничего не давала:
    // замок всё равно нужен, а сотня килобайт на чанк переливалась
    // туда-обратно каждый кадр загрузки.
    {
        std::lock_guard lk(chunk.meshMutex);
        if (!chunk.meshes[lod].built) return false;
        buildChunkVertices(chunk, chunk.meshes[lod].quads,
                           scratchVerts_, scratchIndices_, gm.opaqueIndices,
                           &gm.blendCenter, lod);
        // По каким вокселям построено то, что сейчас поедет в
        // видеопамять. Отсюда потом видно, какие ДРУГИЕ уровни
        // устарели, а какие построены по тем же вокселям и годятся.
        gm.revision = chunk.meshes[lod].revision;
        chunk.meshes[lod].ready.store(false, std::memory_order_release);
    }

    // Разбор первых нескольких мешей в журнал. По картинке нельзя
    // отличить «граней не построилось» от «построились, но ушли не в
    // тот проход»: и то и другое выглядит как мир, вывернутый
    // наизнанку. А по числам — можно.
    //
    // Раньше здесь печатались только границы «небо min..max»: на целый
    // чанк они почти всегда 0..7 и не значат ничего. Считать нужно
    // РАСПРЕДЕЛЕНИЕ, и считать по верхним граням — это они видны с
    // поверхности. Открытая земля обязана давать небо 7; если её
    // грани уходят в младшие значения, освещение всего мира
    // проваливается ровно во столько раз, во сколько мал множитель
    // 0.18 + 0.82 * небо/7, а из общего сумрака торчат отдельные
    // правильно посчитанные блоки.
    {
        static int logged = 0;
        if (logged < 6) {
            ++logged;
            usize skyHist[8] = {}, aoHist[4] = {};
            usize topSky[8] = {}, topCount = 0;
            std::lock_guard lk(chunk.meshMutex);
            for (const auto& q : chunk.meshes[lod].quads) {
                const bool up = (q.v0.face == 2);   // +Y, см. world::FACES
                for (u8 v : q.sky) {
                    ++skyHist[v & 7];
                    if (up) { ++topSky[v & 7]; ++topCount; }
                }
                for (u8 v : q.ao) ++aoHist[v & 3];
            }
            LOGI("меш чанка %d,%d ур.%u: квадов %zu, вершин %zu, индексов %zu "
                 "(непрозрачных %u, полупрозрачных %zu)",
                 chunk.coord.x, chunk.coord.z, (unsigned)lod,
                 chunk.meshes[lod].quads.size(), scratchVerts_.size(),
                 scratchIndices_.size(), gm.opaqueIndices,
                 scratchIndices_.size() - gm.opaqueIndices);
            LOGI("  небо 0..7: %zu %zu %zu %zu %zu %zu %zu %zu; AO 0..3: %zu %zu %zu %zu",
                 skyHist[0], skyHist[1], skyHist[2], skyHist[3],
                 skyHist[4], skyHist[5], skyHist[6], skyHist[7],
                 aoHist[0], aoHist[1], aoHist[2], aoHist[3]);
            LOGI("  верхние грани, небо 0..7: %zu %zu %zu %zu %zu %zu %zu %zu"
                 " (углов %zu, доля открытых %.2f)",
                 topSky[0], topSky[1], topSky[2], topSky[3],
                 topSky[4], topSky[5], topSky[6], topSky[7], topCount,
                 topCount ? (double)topSky[7] / (double)topCount : 0.0);
        }
    }

    if (scratchIndices_.empty()) {
        // Чанк целиком пустой (небо или толща камня внутри) — рисовать нечего.
        gm.totalIndices = 0;
        gm.opaqueIndices = 0;
        gm.uploaded = true;
        return true;
    }

    // Индекс в 16 бит, пока вершин меньше 65536. Для чанка 32x128x32
    // после жадного слияния это практически всегда так, а трафик и
    // видеопамять под индексы сразу вдвое меньше.
    const bool narrow = scratchVerts_.size() <= 65535;
    gm.indexType = narrow ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

    const void* ibSrc = scratchIndices_.data();
    u64 ibBytes = scratchIndices_.size() * sizeof(u32);
    if (narrow) {
        scratchIndices16_.resize(scratchIndices_.size());
        for (usize i = 0; i < scratchIndices_.size(); ++i)
            scratchIndices16_[i] = (u16)scratchIndices_[i];
        ibSrc   = scratchIndices16_.data();
        ibBytes = scratchIndices16_.size() * sizeof(u16);
    }
    const u64 vbBytes = scratchVerts_.size() * sizeof(VoxelVertex);

    if (gm.valid && (gm.vb.size() < vbBytes || gm.ib.size() < ibBytes)) {
        retire(gm.vb); retire(gm.ib);
        gm.valid = false;
        // Буферов больше нет: если пересоздать их не удастся, уровень
        // должен считаться незагруженным, иначе его никто не закажет
        // заново и чанк останется дырой.
        gm.uploaded = false;
    }
    if (!gm.valid) {
        // С запасом 25%, чтобы мелкие правки блоков не пересоздавали буфер.
        const u64 vbCap = vbBytes + vbBytes / 4;
        const u64 ibCap = ibBytes + ibBytes / 4;
        if (!gm.vb.create(dev_, phys_, vbCap, vk::BufferUsage::Vertex, false)) return false;
        if (!gm.ib.create(dev_, phys_, ibCap, vk::BufferUsage::Index, false)) {
            gm.vb.destroy();
            return false;
        }
        gm.valid = true;
    }

    auto* sVb = staging_.acquire(vbBytes);
    if (!sVb) return false;
    auto* sIb = staging_.acquire(ibBytes);
    if (!sIb) { staging_.retire(sVb); return false; }

    std::memcpy(sVb->mapped, scratchVerts_.data(), vbBytes);
    std::memcpy(sIb->mapped, ibSrc, ibBytes);

    VkBufferCopy c1{0, 0, vbBytes};
    vkCmdCopyBuffer(cmd, sVb->buffer, gm.vb.handle(), 1, &c1);
    VkBufferCopy c2{0, 0, ibBytes};
    vkCmdCopyBuffer(cmd, sIb->buffer, gm.ib.handle(), 1, &c2);

    // Staging возвращается в оборот не сразу: после endTransferBatch()
    // вызывающий помечает его сроком годности, и он освободится, когда
    // GPU наверняка дочитает.
    pendingStaging_.push_back(sVb);
    pendingStaging_.push_back(sIb);

    gm.totalIndices = (u32)scratchIndices_.size();
    gm.uploaded = true;
    return true;
}

/// Освобождает список квадов уровня: он уже в видеопамяти.
static void releaseQuads(world::Chunk& chunk, u8 lod) {
    std::lock_guard lk(chunk.meshMutex);
    chunk.meshes[lod].built = false;
    std::vector<world::Quad>().swap(chunk.meshes[lod].quads);
}

// ============================================================
// Кадровая загрузка. Раньше каждая пара буферов уходила отдельным
// submitOneShot с ожиданием fence — восемь полных остановок GPU на
// чанк. Теперь все копии кадра идут одним пакетом.
// ============================================================
void ChunkRenderer::uploadChunks(vk::Context& ctx, world::ChunkManager& world,
                                 const std::vector<world::MeshReady>& ready,
                                 const glm::vec3& cameraPos)
{
    frameNo_ = ctx.framesPresented();
    collectRetired();

    // Пакет передачи заканчивается ожиданием на заборе — это полная
    // остановка GPU. Открывать его «на всякий случай» нельзя: рендер
    // заказывает смену уровня детализации каждый кадр, пока меш не
    // приехал, и очередь запросов почти никогда не пуста. Из-за этого
    // остановка случалась ежекадрово и съедала пятнадцать миллисекунд
    // из шестнадцати.
    //
    // Поэтому сначала выясняем, есть ли что грузить на самом деле:
    // готовый меш нужного уровня. Если нет — только просим мир его
    // построить и уходим, ничего не открывая.
    servable_.clear();
    for (const auto& req : lodRequests_) {
        auto it = meshes_.find(req.coord);
        if (it == meshes_.end() || !it->second.chunk) continue;
        world::Chunk& c = *it->second.chunk;
        if (c.removed.load(std::memory_order_acquire)) continue;

        bool have = false;
        {
            std::lock_guard lk(c.meshMutex);
            have = c.meshes[req.lod].built;
        }
        if (have) {
            if (servable_.size() < MAX_LOD_UPLOADS_PER_FRAME) servable_.push_back(req);
        } else {
            // Заказ повторяем, если прежний так и не приехал.
            //
            // Отметка «этот уровень уже заказан» раньше не имела срока
            // годности, и любая потерянная задача превращалась в
            // вечную дыру: рендер каждый кадр просил уровень, а здесь
            // видел, что он «уже заказан», и не делал ничего. Задача
            // теряется законно — например, если воксели изменились
            // между постановкой и запуском, и она вышла как
            // устаревшая, а новую поставить было некому.
            const bool fresh = it->second.requestedLod == req.lod &&
                               frameNo_ < it->second.requestedFrame + LOD_REQUEST_RETRY;
            if (!fresh) {
                world.requestLod(req.coord, req.lod);
                it->second.requestedLod   = req.lod;
                it->second.requestedFrame = frameNo_;
            }
        }
    }
    lodRequests_.clear();

    if (ready.empty() && servable_.empty()) return;

    VkCommandBuffer cmd = ctx.beginTransferBatch();
    if (cmd == VK_NULL_HANDLE) return;
    // Новый пакет: staging-буферы, из которых GPU уже дочитал, снова
    // свободны. Срок — во столько же пакетов, во сколько кольцо
    // командных буферов передачи.
    staging_.collect(vk::Context::TRANSFER_SLOTS);

    constexpr f32 CH = (f32)world::CHUNK_SIZE;
    constexpr f32 CY = (f32)world::CHUNK_SIZE_Y;

    // 1. Свежепостроенные меши. Завершение адресовано КОНКРЕТНОМУ
    //    уровню — он приехал вместе с чанком, и грузится ровно он.
    //
    //    Здесь стоял перебор «первый built из 0..3», если нужного
    //    уровня не оказалось. Набор построенных уровней меняется во
    //    времени сам по себе, поэтому такой перебор назначал
    //    резидентным то LOD0, то LOD3 при неподвижной камере, и
    //    дальний рельеф перещёлкивал между разрешениями. Угадывать
    //    больше нечего: мир говорит, что построил.
    for (const auto& rm : ready) {
        if (!rm.chunk) continue;
        world::Chunk& c = *rm.chunk;
        if (c.removed.load(std::memory_order_acquire)) continue;
        const u8 arrived = rm.lod & 3;
        const world::ChunkCoord key{ c.coord.x, c.coord.z };

        auto& gpu = meshes_[key];
        gpu.chunk = rm.chunk;   // держим данные: пригодятся для смены LOD

        const glm::vec3 center{ (f32)key.x * CH + CH * 0.5f,
                                CY * 0.5f,
                                (f32)key.z * CH + CH * 0.5f };
        const glm::vec3 d = center - cameraPos;
        gpu.targetLod = lodForDistanceSq(glm::dot(d, d), gpu.residentLod);

        if (uploadLod(ctx, cmd, gpu, c, arrived)) {
            // Устарели не «все прочие», а только построенные по более
            // старым вокселям. Одновременно строящихся уровней у
            // чанка может быть несколько, и все — по одним и тем же
            // вокселям; выбрасывать их из-за чужого завершения значит
            // оставлять чанк без запасного представления ровно в тот
            // миг, когда оно нужнее всего.
            const u64 rev = gpu.lod[arrived].revision;
            for (u8 l = 0; l < 4; ++l) {
                if (l == arrived) continue;
                if (gpu.lod[l].revision >= rev) continue;
                gpu.lod[l].totalIndices  = 0;
                gpu.lod[l].opaqueIndices = 0;
                gpu.lod[l].uploaded      = false;
            }
            gpu.residentLod  = residentAfterUpload(gpu.residentLod,
                                                   gpu.targetLod, arrived);
            gpu.requestedLod = LOD_NONE;
            releaseQuads(c, arrived);
        }
        if (gpu.residentLod != gpu.targetLod && lodRequests_.size() < 256)
            lodRequests_.push_back({ key, gpu.targetLod });
    }

    // 2. Уровни, меш которых уже построен.
    for (const auto& req : servable_) {
        auto it = meshes_.find(req.coord);
        if (it == meshes_.end() || !it->second.chunk) continue;
        ChunkGpu& gpu = it->second;
        world::Chunk& c = *gpu.chunk;
        if (uploadLod(ctx, cmd, gpu, c, req.lod)) {
            // Резидентным приехавший уровень становится по тому же
            // единственному правилу: только если он и есть целевой,
            // либо рисовать было нечем вовсе.
            gpu.residentLod  = residentAfterUpload(gpu.residentLod,
                                                   gpu.targetLod, req.lod);
            gpu.requestedLod = LOD_NONE;
            releaseQuads(c, req.lod);
        }
    }

    ctx.endTransferBatch();

    // Пакет отправлен, но не дождан: GPU ещё читает из staging.
    // Буферы вернутся в оборот сами, через несколько пакетов.
    for (auto* s : pendingStaging_) staging_.retire(s);
    pendingStaging_.clear();
}

// ============================================================
// Отрисовка: отбор, сортировка, два прохода.
//
// Проходы разнесены по двум вызовам, и это не косметика. Между
// непрозрачным ландшафтом и полупрозрачной водой обязаны попасть две
// вещи, которые раньше рисовались ПОСЛЕ воды:
//
//   * непрозрачные сущности — мобы, NPC, предметы, трава. Вода не
//     пишет глубину (иначе смешивание не работает), поэтому моб,
//     нарисованный после неё, проходил проверку глубины и оказывался
//     ПОВЕРХ водной глади, стоя при этом под водой;
//   * небо. Оно закрывает весь экран, и пока оно рисовалось первым,
//     каждый пиксель считал его шейдер, даже если поверх ложился
//     ландшафт. Замер (tools/gpubench): шейдер неба в десять раз
//     дороже простой заливки той же площади, а кадр vkcheck
//     показывает, что геометрия закрывает под две трети экрана.
//
// Порядок в RenderSystem::render теперь такой: ландшафт, сущности,
// трава, небо, вода, контур, интерфейс.
// ============================================================
void ChunkRenderer::drawMesh(VkCommandBuffer cmd, VkPipelineLayout layout,
                             const Visible& v, u32 first, u32 count)
{
    if (count == 0) return;
    const ChunkPush push{ glm::vec4(v.origin, 0.f) };
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(push), &push);
    const VkBuffer vb = v.mesh->vb.handle();
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, offsets);
    vkCmdBindIndexBuffer(cmd, v.mesh->ib.handle(), 0, v.mesh->indexType);
    vkCmdDrawIndexed(cmd, count, 1, first, 0, 0);
}

// ============================================================
// Отбор. Вынесен из renderOpaque отдельной функцией: те же решения
// нужны и когда проход ландшафта выключен ради замера (см.
// cullOnly в заголовке). Рисование отсюда убрано целиком — здесь
// только выбор геометрии и счётчики.
// ============================================================
void ChunkRenderer::cull(const math::Frustum& frustum, const glm::vec3& cameraPos)
{
    lastDrawnChunks_   = 0;
    lastDrawnIndices_  = 0;
    lastEmptyChunks_   = 0;
    lastWaitingChunks_ = 0;
    lastConsideredChunks_ = 0;
    lastCulledChunks_     = 0;
    lastDrawnVertices_    = 0;
    for (auto& l : lodCounts_) l = 0;

    constexpr f32 CH = (f32)world::CHUNK_SIZE;
    constexpr f32 CY = (f32)world::CHUNK_SIZE_Y;

    // ---- 1. Отбор ----
    visible_.clear();
    blended_.clear();
    for (auto& [coord, cm] : meshes_) {
        ++lastConsideredChunks_;
        const glm::vec3 cmin{ (f32)coord.x * CH, 0.f, (f32)coord.z * CH };
        const glm::vec3 cmax{ cmin.x + CH, CY, cmin.z + CH };

        math::AABB aabb; aabb.min = cmin; aabb.max = cmax;
        if (!frustum.intersectsAABB(aabb)) { ++lastCulledChunks_; continue; }

        const glm::vec3 center = (cmin + cmax) * 0.5f;
        const glm::vec3 d = center - cameraPos;
        const f32 distSq = glm::dot(d, d);
        const u8 want = lodForDistanceSq(distSq, cm.residentLod);
        // Целевой уровень решает рендер и только он. uploadChunks
        // сверяется с этим полем, решая, имеет ли приехавший уровень
        // право стать резидентным.
        cm.targetLod = want;

        // Нужного уровня нет в видеопамяти — рисуем тем, что есть,
        // и заказываем догрузку на следующий кадр.
        GpuMesh* chosen = nullptr;
        u8 usedLod = want;
        if (cm.lod[want].valid && cm.lod[want].totalIndices > 0) {
            chosen = &cm.lod[want];
        } else {
            // Заказываем по признаку «этого уровня в видеопамяти нет»,
            // а не по residentLod: после неудачной загрузки резидентный
            // уровень мог совпасть с нужным, запрос не уходил, и чанк
            // оставался невидимым до следующей перестройки меша.
            // Пустой чанк при этом помечен выгруженным и не заказывается.
            if (!cm.lod[want].uploaded && lodRequests_.size() < 256)
                lodRequests_.push_back({ coord, want });

            // Пока нужный уровень не приехал — рисуем РЕЗИДЕНТНЫЙ, и
            // только его.
            //
            // Здесь стоял перебор i = 0..3 с выбором первого годного.
            // Набор годных уровней меняется во времени сам по себе, и
            // выбор «первого попавшегося» дёргал чанк между
            // представлениями: нужен третий, резидентен второй, а
            // рисовался нулевой, если тот случайно ещё лежал в памяти.
            // Резидентный — единственный уровень, про который известно,
            // что он загружен целиком и соответствует текущим вокселям.
            if (cm.residentLod < 4) {
                GpuMesh& res = cm.lod[cm.residentLod];
                if (res.valid && res.totalIndices > 0) {
                    chosen  = &res;
                    usedLod = cm.residentLod;
                }
            }
        }
        if (!chosen) {
            // Чанк в кадре, но рисовать нечем ни на одном уровне —
            // это и есть дыра в ландшафте. По картинке она неотличима
            // от «за этим чанком просто нет мира», по числу — вполне.
            ++lastEmptyChunks_;
            if (cm.requestedLod != LOD_NONE) ++lastWaitingChunks_;
            continue;
        }

        visible_.push_back({ chosen, cmin, distSq });
        if (chosen->totalIndices > chosen->opaqueIndices) {
            // Расстояние до ВОДЫ, а не до центра чанка. Центр чанка
            // лежит на половине высоты мира; для пруда на поверхности
            // это шестьдесят с лишним блоков мимо, и порядок
            // смешивания по нему выходил случайный — дальняя вода
            // ложилась поверх ближней, и стык двух чанков с водой
            // читался как ступенька.
            const glm::vec3 wd = cmin + chosen->blendCenter - cameraPos;
            blended_.push_back({ chosen, cmin, glm::dot(wd, wd) });
        }
        ++lastDrawnChunks_;
        lastDrawnIndices_ += chosen->totalIndices;
        lastDrawnVertices_ += chosen->totalIndices / 6 * 4;   // квад = 6 индексов на 4 вершины
        ++lodCounts_[usedLod];
    }
    // blended_ — подмножество visible_: пусто одно, пусто и другое.
    if (visible_.empty()) return;

    // ---- 2. Порядок ----
    //
    // От ближнего к дальнему: ранний тест глубины отбрасывает
    // закрытые фрагменты до фрагментного шейдера, а он здесь стоит
    // больше всего в кадре.
    //
    // farFirst_ разворачивает порядок и нужен только замеру: картинка
    // от него не меняется (геометрия непрозрачная, тест глубины
    // включён), а разница во времени и есть то, что ранний тест
    // сейчас экономит.
    if (farFirst_)
        std::sort(visible_.begin(), visible_.end(),
                  [](const Visible& a, const Visible& b) { return a.distSq > b.distSq; });
    else
        std::sort(visible_.begin(), visible_.end(),
                  [](const Visible& a, const Visible& b) { return a.distSq < b.distSq; });
}

void ChunkRenderer::renderOpaque(vk::Context& ctx,
                                 VkPipeline opaquePipe,
                                 VkPipelineLayout layout,
                                 VkDescriptorSet set, const math::Frustum& frustum,
                                 const glm::vec3& cameraPos)
{
    VkCommandBuffer cmd = ctx.currentCmd();

    VkViewport vp{};
    vp.x = 0.f; vp.y = 0.f;
    vp.width  = (f32)ctx.extent().width;
    vp.height = (f32)ctx.extent().height;
    vp.minDepth = 0.f; vp.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{}; sc.extent = ctx.extent();
    vkCmdSetScissor(cmd, 0, 1, &sc);

    cull(frustum, cameraPos);
    if (visible_.empty()) return;

    // Непрозрачное, от ближнего к дальнему: порядок задал cull().
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, 1, &set, 0, nullptr);
    for (const auto& v : visible_) drawMesh(cmd, layout, v, 0, v.mesh->opaqueIndices);
}

// ============================================================
// Полупрозрачный проход: вода. Отбор и сортировку сделал
// renderOpaque, здесь остаётся только нарисовать — и нарисовать
// ПОСЛЕ всей непрозрачной геометрии и после неба.
// ============================================================
void ChunkRenderer::renderBlended(vk::Context& ctx,
                                  VkPipeline blendPipe,
                                  VkPipelineLayout layout,
                                  VkDescriptorSet set)
{
    // Обратный порядок — единственный, при котором смешивание даёт
    // верный результат: дальняя вода должна лечь под ближнюю.
    //
    // Сортируется отдельный список: порядок непрозрачного прохода
    // здесь не годится, он считан от центров чанков.
    if (blended_.empty()) return;
    VkCommandBuffer cmd = ctx.currentCmd();
    std::sort(blended_.begin(), blended_.end(),
              [](const Blended& a, const Blended& b) { return a.distSq > b.distSq; });

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blendPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, 1, &set, 0, nullptr);
    for (const auto& b : blended_) {
        const Visible v{ b.mesh, b.origin, b.distSq };
        drawMesh(cmd, layout, v, b.mesh->opaqueIndices,
                 b.mesh->totalIndices - b.mesh->opaqueIndices);
    }
}

} // namespace render

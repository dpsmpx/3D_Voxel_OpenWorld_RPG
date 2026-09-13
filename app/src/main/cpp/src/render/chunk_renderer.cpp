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
                           scratchVerts_, scratchIndices_, gm.opaqueIndices);
        chunk.meshes[lod].ready.store(false, std::memory_order_release);
    }

    // Разбор первых нескольких мешей в журнал. По картинке нельзя
    // отличить «граней не построилось» от «построились, но ушли не в
    // тот проход»: и то и другое выглядит как мир, вывернутый
    // наизнанку. А по числам — можно.
    {
        static int logged = 0;
        if (logged < 6) {
            ++logged;
            u8 skyMin = 7, skyMax = 0, aoMin = 3, aoMax = 0;
            usize blended = 0;
            std::lock_guard lk(chunk.meshMutex);
            for (const auto& q : chunk.meshes[lod].quads) {
                for (u8 v : q.sky) { skyMin = v < skyMin ? v : skyMin;
                                     skyMax = v > skyMax ? v : skyMax; }
                for (u8 v : q.ao)  { aoMin = v < aoMin ? v : aoMin;
                                     aoMax = v > aoMax ? v : aoMax; }
            }
            LOGI("меш чанка %d,%d ур.%u: квадов %zu, вершин %zu, индексов %zu "
                 "(непрозрачных %u, полупрозрачных %zu), небо %u..%u, AO %u..%u",
                 chunk.coord.x, chunk.coord.z, (unsigned)lod,
                 chunk.meshes[lod].quads.size(), scratchVerts_.size(),
                 scratchIndices_.size(), gm.opaqueIndices,
                 scratchIndices_.size() - gm.opaqueIndices,
                 (unsigned)skyMin, (unsigned)skyMax,
                 (unsigned)aoMin, (unsigned)aoMax);
            (void)blended;
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
                                 const std::vector<std::shared_ptr<world::Chunk>>& chunks,
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
        } else if (it->second.requestedLod != req.lod) {
            world.requestLod(req.coord, req.lod);
            it->second.requestedLod = req.lod;
        }
    }
    lodRequests_.clear();

    if (chunks.empty() && servable_.empty()) return;

    VkCommandBuffer cmd = ctx.beginTransferBatch();
    if (cmd == VK_NULL_HANDLE) return;
    // Новый пакет: staging-буферы, из которых GPU уже дочитал, снова
    // свободны. Срок — во столько же пакетов, во сколько кольцо
    // командных буферов передачи.
    staging_.collect(vk::Context::TRANSFER_SLOTS);

    constexpr f32 CH = (f32)world::CHUNK_SIZE;
    constexpr f32 CY = (f32)world::CHUNK_SIZE_Y;

    // 1. Свежепостроенные чанки: грузим только тот LOD, который
    //    понадобится с текущей дистанции, а не все четыре.
    for (const auto& sp : chunks) {
        if (!sp) continue;
        world::Chunk& c = *sp;
        const world::ChunkCoord key{ c.coord.x, c.coord.z };

        auto& gpu = meshes_[key];
        gpu.chunk = sp;   // держим данные: пригодятся для смены LOD

        const glm::vec3 center{ (f32)key.x * CH + CH * 0.5f,
                                CY * 0.5f,
                                (f32)key.z * CH + CH * 0.5f };
        const glm::vec3 d = center - cameraPos;
        const u8 want = lodForDistanceSq(glm::dot(d, d), gpu.residentLod);

        // Мешер строит один уровень, и это мог быть не тот, который
        // нужен сейчас: игрок успел отойти, и lodWanted сменился уже
        // после того, как задача прочитала его. Поэтому берём не
        // «желаемый» уровень, а тот, который действительно построен.
        // Раньше здесь подставлялось текущее значение lodWanted без
        // проверки — и если построен был другой уровень, загрузка
        // тихо возвращала false.
        constexpr u8 NONE = 0xFF;
        u8 have = NONE;
        {
            std::lock_guard lk(c.meshMutex);
            if (c.meshes[want].built) {
                have = want;
            } else {
                const u8 w = c.lodWanted.load(std::memory_order_acquire) & 3;
                if (c.meshes[w].built) have = w;
                else for (u8 l = 0; l < 4; ++l)
                    if (c.meshes[l].built) { have = l; break; }
            }
        }

        if (have != NONE && uploadLod(ctx, cmd, gpu, c, have)) {
            // Остальные уровни устарели вместе с перестроенным мешем —
            // но обнулять их можно только теперь, когда на смену
            // действительно приехал новый. Раньше они обнулялись до
            // загрузки: не удалась загрузка — и чанк пропадал с экрана
            // целиком, потому что рисовать стало нечем ни на одном
            // уровне. На ходу это выглядело как дыры в мире.
            for (u8 l = 0; l < 4; ++l) {
                if (l == have) continue;
                gpu.lod[l].totalIndices  = 0;
                gpu.lod[l].opaqueIndices = 0;
                gpu.lod[l].uploaded      = false;
            }
            gpu.residentLod  = have;
            gpu.requestedLod = 0xFF;
            releaseQuads(c, have);
        }
        if (have != want && lodRequests_.size() < 256)
            lodRequests_.push_back({ key, want });
    }

    // 2. Уровни, меш которых уже построен.
    for (const auto& req : servable_) {
        auto it = meshes_.find(req.coord);
        if (it == meshes_.end() || !it->second.chunk) continue;
        world::Chunk& c = *it->second.chunk;
        if (uploadLod(ctx, cmd, it->second, c, req.lod)) {
            it->second.residentLod  = req.lod;
            it->second.requestedLod = 0xFF;
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
// ============================================================
void ChunkRenderer::render(vk::Context& ctx,
                           VkPipeline opaquePipe, VkPipeline blendPipe,
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

    lastDrawnChunks_  = 0;
    lastDrawnIndices_ = 0;
    for (auto& l : lodCounts_) l = 0;

    constexpr f32 CH = (f32)world::CHUNK_SIZE;
    constexpr f32 CY = (f32)world::CHUNK_SIZE_Y;

    // ---- 1. Отбор ----
    visible_.clear();
    for (auto& [coord, cm] : meshes_) {
        const glm::vec3 cmin{ (f32)coord.x * CH, 0.f, (f32)coord.z * CH };
        const glm::vec3 cmax{ cmin.x + CH, CY, cmin.z + CH };

        math::AABB aabb; aabb.min = cmin; aabb.max = cmax;
        if (!frustum.intersectsAABB(aabb)) continue;

        const glm::vec3 center = (cmin + cmax) * 0.5f;
        const glm::vec3 d = center - cameraPos;
        const f32 distSq = glm::dot(d, d);
        const u8 want = lodForDistanceSq(distSq, cm.residentLod);

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
            for (u8 i = 0; i < 4; ++i) {
                if (cm.lod[i].valid && cm.lod[i].totalIndices > 0) {
                    chosen = &cm.lod[i];
                    usedLod = i;
                    break;
                }
            }
        }
        if (!chosen) continue;

        visible_.push_back({ chosen, cmin, distSq });
        ++lastDrawnChunks_;
        lastDrawnIndices_ += chosen->totalIndices;
        ++lodCounts_[usedLod];
    }
    if (visible_.empty()) return;

    // ---- 2. Порядок ----
    std::sort(visible_.begin(), visible_.end(),
              [](const Visible& a, const Visible& b) { return a.distSq < b.distSq; });

    // ---- 3. Непрозрачное, от ближнего к дальнему ----
    auto bindAndDraw = [&](const Visible& v, u32 first, u32 count) {
        if (count == 0) return;
        const ChunkPush push{ glm::vec4(v.origin, 0.f) };
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(push), &push);
        const VkBuffer vb = v.mesh->vb.handle();
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, offsets);
        vkCmdBindIndexBuffer(cmd, v.mesh->ib.handle(), 0, v.mesh->indexType);
        vkCmdDrawIndexed(cmd, count, 1, first, 0, 0);
    };

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, 1, &set, 0, nullptr);
    for (const auto& v : visible_) bindAndDraw(v, 0, v.mesh->opaqueIndices);

    // ---- 4. Полупрозрачное, от дальнего к ближнему ----
    // Обратный порядок — единственный, при котором смешивание даёт
    // верный результат: дальняя вода должна лечь под ближнюю.
    bool blendBound = false;
    for (auto it = visible_.rbegin(); it != visible_.rend(); ++it) {
        const u32 count = it->mesh->totalIndices - it->mesh->opaqueIndices;
        if (count == 0) continue;
        if (!blendBound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blendPipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                    0, 1, &set, 0, nullptr);
            blendBound = true;
        }
        bindAndDraw(*it, it->mesh->opaqueIndices, count);
    }
}

} // namespace render

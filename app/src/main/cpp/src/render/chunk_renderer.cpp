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
    staging_.destroy();
}

void ChunkRenderer::forgetChunk(world::ChunkCoord c) {
    auto it = meshes_.find(c);
    if (it == meshes_.end()) return;
    for (auto& g : it->second.lod)
        if (g.valid) { g.vb.destroy(); g.ib.destroy(); }
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

    if (scratchIndices_.empty()) {
        // Чанк целиком пустой (небо или толща камня внутри) — рисовать нечего.
        gm.totalIndices = 0;
        gm.opaqueIndices = 0;
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
        gm.vb.destroy(); gm.ib.destroy(); gm.valid = false;
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
    if (!sIb) { staging_.release(sVb); return false; }

    std::memcpy(sVb->mapped, scratchVerts_.data(), vbBytes);
    std::memcpy(sIb->mapped, ibSrc, ibBytes);

    VkBufferCopy c1{0, 0, vbBytes};
    vkCmdCopyBuffer(cmd, sVb->buffer, gm.vb.handle(), 1, &c1);
    VkBufferCopy c2{0, 0, ibBytes};
    vkCmdCopyBuffer(cmd, sIb->buffer, gm.ib.handle(), 1, &c2);

    // Staging освобождается вызывающим после endTransferBatch():
    // до этого GPU ещё читает из него.
    pendingStaging_.push_back(sVb);
    pendingStaging_.push_back(sIb);

    gm.totalIndices = (u32)scratchIndices_.size();
    return true;
}

// ============================================================
// Кадровая загрузка. Раньше каждая пара буферов уходила отдельным
// submitOneShot с ожиданием fence — восемь полных остановок GPU на
// чанк. Теперь все копии кадра идут одним пакетом.
// ============================================================
void ChunkRenderer::uploadChunks(vk::Context& ctx,
                                 const std::vector<std::shared_ptr<world::Chunk>>& chunks,
                                 const glm::vec3& cameraPos)
{
    if (chunks.empty() && lodRequests_.empty()) return;

    VkCommandBuffer cmd = ctx.beginTransferBatch();
    if (cmd == VK_NULL_HANDLE) return;

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
        const u8 want = lodForDistanceSq(glm::dot(d, d));

        // Остальные уровни помечаем недействительными: их содержимое
        // устарело вместе с перестроенным мешем.
        for (u8 l = 0; l < 4; ++l) {
            if (l != want && gpu.lod[l].valid) gpu.lod[l].totalIndices = 0;
        }
        if (uploadLod(ctx, cmd, gpu, c, want)) gpu.residentLod = want;
    }

    // 2. Отложенные запросы смены детализации от render().
    u32 served = 0;
    for (const auto& req : lodRequests_) {
        if (served >= MAX_LOD_UPLOADS_PER_FRAME) break;
        auto it = meshes_.find(req.coord);
        if (it == meshes_.end() || !it->second.chunk) continue;
        if (it->second.chunk->removed.load(std::memory_order_acquire)) continue;
        if (uploadLod(ctx, cmd, it->second, *it->second.chunk, req.lod)) {
            it->second.residentLod = req.lod;
            ++served;
        }
    }
    lodRequests_.clear();

    ctx.endTransferBatch();

    for (auto* s : pendingStaging_) staging_.release(s);
    pendingStaging_.clear();
}

// ============================================================
// Отрисовка: отбор, сортировка, два прохода.
// ============================================================
void ChunkRenderer::render(vk::Context& ctx,
                           VkPipeline opaquePipe, VkPipeline blendPipe,
                           VkPipelineLayout layout,
                           VkDescriptorSet set, const math::Frustum& frustum,
                           const glm::vec3& cameraPos,
                           OcclusionCuller* occlusion)
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
    if (occlusion) occlusion->resetStats();

    constexpr f32 CH = (f32)world::CHUNK_SIZE;
    constexpr f32 CY = (f32)world::CHUNK_SIZE_Y;

    // ---- 1. Отбор ----
    visible_.clear();
    for (auto& [coord, cm] : meshes_) {
        const glm::vec3 cmin{ (f32)coord.x * CH, 0.f, (f32)coord.z * CH };
        const glm::vec3 cmax{ cmin.x + CH, CY, cmin.z + CH };

        math::AABB aabb; aabb.min = cmin; aabb.max = cmax;
        if (!frustum.intersectsAABB(aabb)) continue;

        // Отсечение перекрытых чанков: дешевле, чем отправить их
        // на растеризацию и получить полный overdraw.
        if (occlusion && occlusion->isOccluded(coord, cameraPos)) {
            occlusion->countCulled();
            continue;
        }

        const glm::vec3 center = (cmin + cmax) * 0.5f;
        const glm::vec3 d = center - cameraPos;
        const f32 distSq = glm::dot(d, d);
        const u8 want = lodForDistanceSq(distSq);

        // Нужного уровня нет в видеопамяти — рисуем тем, что есть,
        // и заказываем догрузку на следующий кадр.
        GpuMesh* chosen = nullptr;
        u8 usedLod = want;
        if (cm.lod[want].valid && cm.lod[want].totalIndices > 0) {
            chosen = &cm.lod[want];
        } else {
            if (cm.residentLod != want && lodRequests_.size() < 256)
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

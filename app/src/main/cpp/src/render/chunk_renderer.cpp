/**
 * @file chunk_renderer.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "chunk_renderer.h"
#include "../core/log.h"
#include <algorithm>
#include <cstring>

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
    // Копируем квады под замком: задача меширования может как раз
    // перезаписывать этот же LOD.
    {
        std::lock_guard lk(chunk.meshMutex);
        if (!chunk.meshes[lod].built) return false;
        scratchQuads_ = chunk.meshes[lod].quads;
        chunk.meshes[lod].ready.store(false, std::memory_order_release);
    }

    buildChunkVertices(chunk, scratchQuads_, scratchVerts_, scratchIndices_);

    auto& gm = gpu.lod[lod];
    if (scratchIndices_.empty()) {
        // Чанк целиком пустой (небо или толща камня внутри) — рисовать нечего.
        gm.indexCount = 0;
        return true;
    }

    const u64 vbBytes = scratchVerts_.size()   * sizeof(VoxelVertex);
    const u64 ibBytes = scratchIndices_.size() * sizeof(u32);

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

    std::memcpy(sVb->mapped, scratchVerts_.data(),   vbBytes);
    std::memcpy(sIb->mapped, scratchIndices_.data(), ibBytes);

    VkBufferCopy c1{0, 0, vbBytes};
    vkCmdCopyBuffer(cmd, sVb->buffer, gm.vb.handle(), 1, &c1);
    VkBufferCopy c2{0, 0, ibBytes};
    vkCmdCopyBuffer(cmd, sIb->buffer, gm.ib.handle(), 1, &c2);

    // Staging освобождается вызывающим после endTransferBatch():
    // до этого GPU ещё читает из него.
    pendingStaging_.push_back(sVb);
    pendingStaging_.push_back(sIb);

    gm.indexCount = (u32)scratchIndices_.size();
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
            if (l != want && gpu.lod[l].valid) gpu.lod[l].indexCount = 0;
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
// Отрисовка: frustum culling + выбор LOD по дистанции.
// ============================================================
void ChunkRenderer::render(vk::Context& ctx, VkPipeline pipe, VkPipelineLayout layout,
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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, 1, &set, 0, nullptr);

    lastDrawnChunks_  = 0;
    lastDrawnIndices_ = 0;
    for (auto& l : lodCounts_) l = 0;
    if (occlusion) occlusion->resetStats();

    constexpr f32 CH = (f32)world::CHUNK_SIZE;
    constexpr f32 CY = (f32)world::CHUNK_SIZE_Y;

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
        const u8 want = lodForDistanceSq(glm::dot(d, d));

        // Нужного уровня нет в видеопамяти — рисуем тем, что есть,
        // и заказываем догрузку на следующий кадр.
        GpuMesh* chosen = nullptr;
        u8 usedLod = want;
        if (cm.lod[want].valid && cm.lod[want].indexCount > 0) {
            chosen = &cm.lod[want];
        } else {
            if (cm.residentLod != want && lodRequests_.size() < 256)
                lodRequests_.push_back({ coord, want });
            for (u8 i = 0; i < 4; ++i) {
                if (cm.lod[i].valid && cm.lod[i].indexCount > 0) {
                    chosen = &cm.lod[i];
                    usedLod = i;
                    break;
                }
            }
        }
        if (!chosen) continue;

        const VkBuffer vb = chosen->vb.handle();
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, offsets);
        vkCmdBindIndexBuffer(cmd, chosen->ib.handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, chosen->indexCount, 1, 0, 0, 0);

        ++lastDrawnChunks_;
        lastDrawnIndices_ += chosen->indexCount;
        ++lodCounts_[usedLod];
    }
}

} // namespace render

#include "chunk_renderer.h"
#include "../core/log.h"

namespace render {

bool ChunkRenderer::init(VkDevice dev, VkPhysicalDevice phys) {
    dev_ = dev; phys_ = phys;
    staging_.init(dev, phys);
    scratchVerts_.reserve(65536);
    scratchIndices_.reserve(98304);
    return true;
}

void ChunkRenderer::shutdown() {
    for (auto& [_, m] : meshes_)
        for (auto& g : m.lod)
            if (g.valid) { g.vb.destroy(); g.ib.destroy(); }
    meshes_.clear();
    staging_.destroy();
}

void ChunkRenderer::forgetChunk(world::ChunkCoord c) {
    auto it = meshes_.find(c);
    if (it == meshes_.end()) return;
    for (auto& g : it->second.lod)
        if (g.valid) { g.vb.destroy(); g.ib.destroy(); }
    meshes_.erase(it);
}

void ChunkRenderer::uploadChunks(vk::Context& ctx, const std::vector<world::Chunk*>& chunks) {
    for (world::Chunk* c : chunks) {
        if (!c) continue;
        auto& cm = meshes_[c->coord];

        for (u8 lod = 0; lod < 4; ++lod) {
            if (!c->meshes[lod].ready.load(std::memory_order_acquire)) continue;

            buildChunkVertices(*c, (world::Lod)lod, scratchVerts_, scratchIndices_);
            if (scratchIndices_.empty()) {
                c->meshes[lod].ready.store(false, std::memory_order_release);
                continue;
            }

            u64 vbBytes = scratchVerts_.size() * sizeof(VoxelVertex);
            u64 ibBytes = scratchIndices_.size() * sizeof(u32);

            auto& gm = cm.lod[lod];

            // Пересоздать, если старые буферы малы
            if (gm.valid && (gm.vb.size() < vbBytes || gm.ib.size() < ibBytes)) {
                gm.vb.destroy(); gm.ib.destroy(); gm.valid = false;
            }
            if (!gm.valid) {
                if (!gm.vb.create(dev_, phys_, vbBytes, vk::BufferUsage::Vertex, false)) continue;
                if (!gm.ib.create(dev_, phys_, ibBytes, vk::BufferUsage::Index, false)) {
                    gm.vb.destroy(); continue;
                }
                gm.valid = true;
            }

            // Пул staging
            auto* sVb = staging_.acquire(vbBytes);
            auto* sIb = staging_.acquire(ibBytes);
            if (!sVb || !sIb) continue;
            std::memcpy(sVb->mapped, scratchVerts_.data(), vbBytes);
            std::memcpy(sIb->mapped, scratchIndices_.data(), ibBytes);

            ctx.submitOneShot([&](VkCommandBuffer cmd) {
                VkBufferCopy c1{0, 0, vbBytes};
                vkCmdCopyBuffer(cmd, sVb->buffer, gm.vb.handle(), 1, &c1);
                VkBufferCopy c2{0, 0, ibBytes};
                vkCmdCopyBuffer(cmd, sIb->buffer, gm.ib.handle(), 1, &c2);

                VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                    0, 1, &mb, 0, nullptr, 0, nullptr);
            });

            // submitOneShot гарантировал завершение GPU
            staging_.release(sVb);
            staging_.release(sIb);

            gm.indexCount = (u32)scratchIndices_.size();
            c->meshes[lod].ready.store(false, std::memory_order_release);
        }
    }
}

void ChunkRenderer::render(vk::Context& ctx, VkPipeline pipe, VkPipelineLayout layout,
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

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                            0, 1, &set, 0, nullptr);

    lastDrawnChunks_ = 0;
    lastDrawnIndices_ = 0;
    for (auto& l : lodCounts_) l = 0;

    constexpr f32 CH = (f32)world::CHUNK_SIZE;
    constexpr f32 CY = (f32)world::CHUNK_SIZE_Y;

    for (auto& [coord, cm] : meshes_) {
        // AABB чанка
        glm::vec3 cmin{ (f32)coord.x * CH, 0.f, (f32)coord.z * CH };
        glm::vec3 cmax{ cmin.x + CH, CY, cmin.z + CH };

        math::AABB aabb; aabb.min = cmin; aabb.max = cmax;
        if (!frustum.intersectsAABB(aabb)) continue;

        // Выбор LOD
        glm::vec3 center = (cmin + cmax) * 0.5f;
        glm::vec3 d = center - cameraPos;
        f32 distSq = glm::dot(d, d);

        u8 lod;
        if (distSq < LOD0_SQ) lod = 0;
        else if (distSq < LOD1_SQ) lod = 1;
        else if (distSq < LOD2_SQ) lod = 2;
        else lod = 3;

        // Пробуем желаемый LOD, fallback на более детальный
        GpuMesh* chosen = nullptr;
        for (i32 i = (i32)lod; i >= 0; --i) {
            if (cm.lod[i].valid && cm.lod[i].indexCount > 0) { chosen = &cm.lod[i]; lod = (u8)i; break; }
        }
        if (!chosen) continue;

        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, &chosen->vb.handle(), offsets);
        vkCmdBindIndexBuffer(cmd, chosen->ib.handle(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, chosen->indexCount, 1, 0, 0, 0);

        ++lastDrawnChunks_;
        lastDrawnIndices_ += chosen->indexCount;
        ++lodCounts_[lod];
    }
}

} // namespace render
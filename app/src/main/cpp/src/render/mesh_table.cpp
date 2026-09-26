/**
 * @file mesh_table.cpp
 * @brief Рендер: меши воксельных моделей в общем буфере — таблица и вызовы отрисовки.
 *
 * MeshTable::set(vk::Context&) — в voxel_model_renderer.cpp: ему нужен
 * контекст игрового кадра. См. mesh_table.h.
 */
#include "mesh_table.h"
#include "../core/log.h"
#include <vector>

namespace render {

void MeshTable::gather(const std::vector<VoxelMesh>& meshes,
                       std::vector<VoxelModelVertex>& verts, std::vector<u32>& idx)
{
    ranges_.clear();
    ranges_.reserve(meshes.size());
    for (const VoxelMesh& m : meshes) {
        Range r;
        r.firstIndex   = (u32)idx.size();
        r.indexCount   = (u32)m.indices.size();
        r.vertexOffset = (i32)verts.size();
        verts.insert(verts.end(), m.vertices.begin(), m.vertices.end());
        idx.insert(idx.end(), m.indices.begin(), m.indices.end());
        ranges_.push_back(r);
    }
    vbo_.destroy();
    ibo_.destroy();
}

bool MeshTable::setHostVisible(VkDevice dev, VkPhysicalDevice phys,
                               const std::vector<VoxelMesh>& meshes)
{
    std::vector<VoxelModelVertex> verts;
    std::vector<u32> idx;
    gather(meshes, verts, idx);
    if (verts.empty()) return true;
    const u64 vb = (u64)verts.size() * sizeof(VoxelModelVertex);
    const u64 ib = (u64)idx.size() * sizeof(u32);
    if (!vbo_.create(dev, phys, vb, vk::BufferUsage::Vertex, true) ||
        !ibo_.create(dev, phys, ib, vk::BufferUsage::Index, true))
    {
        LOGE("MeshTable: меши моделей не загружены");
        destroy();
        return false;
    }
    vbo_.write(verts.data(), vb);
    ibo_.write(idx.data(), ib);
    return true;
}

void MeshTable::destroy() {
    vbo_.destroy();
    ibo_.destroy();
    ranges_.clear();
}

void MeshTable::bind(VkCommandBuffer cmd, VkBuffer instances) const {
    VkBuffer vbs[2] = { vbo_.handle(), instances };
    VkDeviceSize offs[2] = { 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
    vkCmdBindIndexBuffer(cmd, ibo_.handle(), 0, VK_INDEX_TYPE_UINT32);
}

void MeshTable::draw(VkCommandBuffer cmd, u32 mesh, u32 instanceCount, u32 firstInstance) const {
    if (empty(mesh) || instanceCount == 0) return;
    const Range& r = ranges_[mesh];
    vkCmdDrawIndexed(cmd, r.indexCount, instanceCount, r.firstIndex, r.vertexOffset, firstInstance);
}

} // namespace render

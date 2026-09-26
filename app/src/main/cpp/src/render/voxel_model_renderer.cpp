/**
 * @file voxel_model_renderer.cpp
 * @brief Рендер: воксельные модели из мелких вокселей, по экземпляру на вещь.
 */
#include "voxel_model_renderer.h"
#include "mob_renderer.h"
#include "../core/log.h"
#include <algorithm>
#include <utility>
#include <vector>

namespace render {

namespace {

/// Статичный буфер на видеокарте, заполненный через промежуточный.
bool uploadStatic(vk::Context& ctx, vk::Buffer& dst, vk::BufferUsage usage,
                  const void* data, u64 bytes)
{
    VkDevice dev = ctx.device();
    if (!dst.create(dev, ctx.physicalDevice(), bytes, usage, false)) return false;
    vk::Buffer staging;
    if (!staging.create(dev, ctx.physicalDevice(), bytes, vk::BufferUsage::Staging, true)) {
        dst.destroy();
        return false;
    }
    staging.write(data, bytes);
    ctx.submitOneShot([&](VkCommandBuffer cmd) {
        VkBufferCopy c{ 0, 0, bytes };
        vkCmdCopyBuffer(cmd, staging.handle(), dst.handle(), 1, &c);
    });
    staging.destroy();
    return true;
}

} // namespace

bool VoxelModelRenderer::init(vk::Context& ctx, AAssetManager* mgr,
                              VkDescriptorSetLayout descLayout)
{
    dev_ = ctx.device();
    instances_.init(dev_, ctx.physicalDevice());
    shaders_.init(dev_, mgr);

    vk::PipelineDesc d{};
    d.renderPass  = ctx.renderPass();
    d.descLayout  = descLayout;
    // Освещение — то же, что у существ: модели лежат на тех же
    // блоках и под тем же солнцем.
    d.vertName    = "shaders/voxmodel.vert.spv";
    d.fragName    = "shaders/mob.frag.spv";
    d.depthFormat = ctx.depthFormat();
    d.cullMode    = VK_CULL_MODE_BACK_BIT;
    d.depthTest   = true;
    d.depthWrite  = true;
    d.blend       = false;
    d.bindings     = VOXMODEL_BINDINGS;
    d.bindingCount = VOXMODEL_BINDING_COUNT;
    d.attrs        = VOXMODEL_ATTRS;
    d.attrCount    = VOXMODEL_ATTR_COUNT;
    if (!pipeline_.create(dev_, shaders_, d)) return false;

    pending_.reserve(256);
    sorted_.reserve(256);
    LOGI("VoxelModelRenderer готов");
    return true;
}

bool MeshTable::set(vk::Context& ctx, const std::vector<VoxelMesh>& meshes) {
    std::vector<VoxelModelVertex> verts;
    std::vector<u32> idx;
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
    if (verts.empty()) return true;
    if (!uploadStatic(ctx, vbo_, vk::BufferUsage::Vertex, verts.data(),
                      (u64)verts.size() * sizeof(VoxelModelVertex)) ||
        !uploadStatic(ctx, ibo_, vk::BufferUsage::Index, idx.data(),
                      (u64)idx.size() * sizeof(u32)))
    {
        LOGE("MeshTable: меши моделей не загружены");
        ranges_.clear();
        return false;
    }
    LOGI("MeshTable: мешей %zu, вершин %zu, треугольников %zu",
         meshes.size(), verts.size(), idx.size() / 3);
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

void VoxelModelRenderer::add(u32 model, const glm::vec3& pos, const glm::vec4& rot,
                             f32 scale, u32 tintRgba)
{
    VoxelModelInstance inst{};
    inst.pos = pos;
    inst.scale = scale;
    inst.rot = rot;
    inst.tintGpu = packInstanceColor(tintRgba);
    pending_.push_back({ model, inst });
}

void VoxelModelRenderer::batch(std::vector<std::pair<u32, VoxelModelInstance>>& pending,
                               std::vector<VoxelModelInstance>& sorted,
                               std::vector<Draw>& draws)
{
    std::stable_sort(pending.begin(), pending.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    sorted.clear();
    draws.clear();
    for (const auto& [model, inst] : pending) {
        if (draws.empty() || draws.back().model != model)
            draws.push_back({ model, (u32)sorted.size(), 0 });
        ++draws.back().count;
        sorted.push_back(inst);
    }
}

void VoxelModelRenderer::upload(vk::Context& ctx) {
    (void)ctx;
    // Модели без меша (номер вне таблицы) рисовать нечем.
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const auto& p) { return meshes_.empty(p.first); }),
                   pending_.end());
    batch(pending_, sorted_, draws_);
    instanceCount_ = (u32)sorted_.size();
    if (instanceCount_ == 0) return;
    if (!instances_.write(sorted_.data(), (u64)instanceCount_ * sizeof(VoxelModelInstance))) {
        instanceCount_ = 0;
        draws_.clear();
    }
}

void VoxelModelRenderer::render(vk::Context& ctx, VkDescriptorSet set) {
    if (instanceCount_ == 0 || !instances_.handle() || !pipeline_.valid() || !meshes_.valid())
        return;
    VkCommandBuffer cmd = ctx.currentCmd();
    ctx.setFullViewport(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_.layout(), 0, 1, &set, 0, nullptr);
    meshes_.bind(cmd, instances_.handle());
    for (const Draw& d : draws_) meshes_.draw(cmd, d.model, d.count, d.firstInstance);
}

void VoxelModelRenderer::destroy() {
    meshes_.destroy();
    instances_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render

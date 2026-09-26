/**
 * @file flora_renderer.cpp
 * @brief Рендер: растения и мелкие природные вещи — экземплярами общих моделей.
 */
#include "flora_renderer.h"
#include "chunk_renderer.h"
#include "flora_models.h"
#include "../core/log.h"
#include <chrono>
#include <vector>

namespace render {

using world::FloraInstance;

bool FloraRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
    instances_.init(dev_, ctx.physicalDevice());
    shaders_.init(dev_, mgr);

    const vk::PipelineDesc d = floraPipelineDesc(ctx.renderPass(), descLayout, ctx.depthFormat());
    if (!pipeline_.create(dev_, shaders_, d)) return false;

    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<VoxelMesh> meshes = buildFloraMeshes();
    const f64 ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!meshes_.set(ctx, meshes)) return false;
    meshQuads_.resize(meshes.size());
    for (usize i = 0; i < meshes.size(); ++i) meshQuads_[i] = meshes[i].quadCount();
    LOGI("FloraRenderer готов: моделей %u, мешей %zu, собраны за %.1f мс",
         floraModelCount(), meshes.size(), ms);
    return true;
}

void FloraRenderer::render(vk::Context& ctx, VkDescriptorSet set, const ChunkRenderer& chunks,
                           const glm::vec3& cameraPos)
{
    batch_.begin(cameraPos);
    chunks.forEachVisibleFlora([&](const glm::vec3& origin, const std::vector<FloraInstance>& list) {
        batch_.addChunk(origin, list);
    });
    batch_.finish(&meshQuads_);

    const auto& inst = batch_.instances();
    if (inst.empty() || batch_.draws().empty() || !pipeline_.valid() || !meshes_.valid()) return;
    if (!instances_.write(inst.data(), (u64)inst.size() * sizeof(FloraGpuInstance))) return;

    VkCommandBuffer cmd = ctx.currentCmd();
    ctx.setFullViewport(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.layout(), 0, 1, &set,
                            0, nullptr);
    meshes_.bind(cmd, instances_.handle());
    for (const FloraBatch::Draw& dr : batch_.draws()) meshes_.draw(cmd, dr.mesh, dr.count, dr.first);
}

void FloraRenderer::destroy() {
    meshes_.destroy();
    instances_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render

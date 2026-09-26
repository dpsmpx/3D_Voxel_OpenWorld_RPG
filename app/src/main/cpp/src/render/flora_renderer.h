/**
 * @file flora_renderer.h
 * @brief Рендер: растения и мелкие природные вещи — экземплярами общих моделей.
 *
 * Сборка кадра — render/flora_batch.h; здесь конвейер, меши в
 * видеопамяти и вызовы отрисовки.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_context.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "flora_batch.h"
#include "flora_pipeline.h"
#include "instance_ring.h"
#include "voxel_model_renderer.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <cstddef>
#include <vector>

namespace render {

class ChunkRenderer;

class FloraRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    /// Собрать экземпляры видимых чанков и нарисовать. Зовётся сразу
    /// после непрозрачного ландшафта: список видимого готовит он.
    void render(vk::Context& ctx, VkDescriptorSet set, const ChunkRenderer& chunks,
                const glm::vec3& cameraPos);

    u32 instanceCount() const { return (u32)batch_.instances().size(); }
    u32 drawCount() const { return (u32)batch_.draws().size(); }
    u32 quadCount() const { return batch_.quads(); }

private:
    VkDevice             dev_ = VK_NULL_HANDLE;
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;
    MeshTable            meshes_;
    std::vector<u32>     meshQuads_;
    InstanceRing         instances_;
    FloraBatch           batch_;
};

} // namespace render

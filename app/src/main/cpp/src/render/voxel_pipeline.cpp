/**
 * @file voxel_pipeline.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "voxel_pipeline.h"
#include "mesh_builder.h"

namespace render {

// Вершинный формат террейна — см. VoxelVertex в mesh_builder.h.
static const vk::VertexBinding kVoxelBindings[1] = { { sizeof(VoxelVertex), false } };
static const vk::VertexAttr kVoxelAttrs[2] = {
    { 0, 0, VK_FORMAT_R32_UINT,       0 },   // inPacked: позиция, грань, AO, зерно
    { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 4 },   // inColor: цвет материала грани
};

vk::PipelineDesc voxelPipelineDesc(VkRenderPass rp, VkDescriptorSetLayout layout,
                                   VkFormat depthFormat)
{
    vk::PipelineDesc d{};
    d.renderPass  = rp;
    d.descLayout  = layout;
    d.vertName    = "shaders/voxel.vert.spv";
    d.fragName    = "shaders/voxel.frag.spv";
    d.depthFormat = depthFormat;
    d.cullMode    = VK_CULL_MODE_BACK_BIT;
    d.depthTest   = true;
    d.depthWrite  = true;
    d.blend       = false;
    d.bindings     = kVoxelBindings;
    d.bindingCount = 1;
    d.attrs        = kVoxelAttrs;
    d.attrCount    = 2;
    d.pushConstantSize  = sizeof(ChunkPush);
    d.pushConstantStage = VK_SHADER_STAGE_VERTEX_BIT;
    return d;
}

// Полупрозрачный проход. Глубину читаем, но не пишем: две поверхности
// воды подряд иначе вырезают друг друга, и в озере появляются дыры.
// Грани не отсекаем — на поверхность воды смотрят и снизу.
void makeVoxelBlendDesc(vk::PipelineDesc& d) {
    d.blend      = true;
    d.depthWrite = false;
    d.cullMode   = VK_CULL_MODE_NONE;
}

} // namespace render

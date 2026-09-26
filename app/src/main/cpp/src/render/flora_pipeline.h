/**
 * @file flora_pipeline.h
 * @brief Рендер: вершинный формат и описание конвейера растений.
 *
 * Одно на игровой кадр (FloraRenderer) и на изометрический снимок
 * (IsoSnapshot): у снимка свой проход рендера, а растения на нём
 * обязаны выглядеть так же, как в игре. Только описание — без
 * контекста игрового кадра, поэтому годится и рендеру на голых
 * ручках Vulkan.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_pipeline.h"
#include "flora_batch.h"
#include "voxel_model.h"
#include <cstddef>

namespace render {

static const vk::VertexBinding FLORA_BINDINGS[2] = {
    { sizeof(VoxelModelVertex), false },
    { sizeof(FloraGpuInstance), true  },
};
static const vk::VertexAttr FLORA_ATTRS[5] = {
    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0  },   // inPos
    { 1, 0, VK_FORMAT_R8G8B8A8_UNORM,   12 },   // inColor
    { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, 16 },   // inNormal
    { 3, 1, VK_FORMAT_R32G32B32_SFLOAT, 0  },   // iPos
    { 4, 1, VK_FORMAT_R8G8B8A8_UNORM,   12 },   // iParams
};
static_assert(offsetof(FloraGpuInstance, pos)    == 0);
static_assert(offsetof(FloraGpuInstance, params) == 12);

/// Конвейер растений: вершины модели и экземпляры, свет существ
/// (mob.frag) — растения стоят на тех же блоках под тем же солнцем.
inline vk::PipelineDesc floraPipelineDesc(VkRenderPass renderPass,
                                          VkDescriptorSetLayout descLayout,
                                          VkFormat depthFormat)
{
    vk::PipelineDesc d{};
    d.renderPass   = renderPass;
    d.descLayout   = descLayout;
    d.vertName     = "shaders/flora.vert.spv";
    d.fragName     = "shaders/mob.frag.spv";
    d.depthFormat  = depthFormat;
    d.cullMode     = VK_CULL_MODE_BACK_BIT;
    d.depthTest    = true;
    d.depthWrite   = true;
    d.blend        = false;
    d.bindings     = FLORA_BINDINGS;
    d.bindingCount = 2;
    d.attrs        = FLORA_ATTRS;
    d.attrCount    = 5;
    return d;
}

} // namespace render

/**
 * @file voxel_pipeline.h
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#pragma once
#include "../vk/vk_pipeline.h"

namespace render {

/// Описание конвейера террейна — одно на игру и на офлайн-проверку.
///
/// Вынесено из RenderSystem::init затем, что проверка графики
/// (tools/vkcheck) обязана собирать ТОТ ЖЕ конвейер: раскладка
/// вершины, отсечение граней, направление обхода, глубина,
/// смешивание — ровно те поля, расхождение в которых видно только на
/// устройстве. Пока числа жили внутри RenderSystem, повторить их без
/// всего движка было нечем, и проверка сверяла бы свою копию с самой
/// собой.
vk::PipelineDesc voxelPipelineDesc(VkRenderPass rp, VkDescriptorSetLayout layout,
                                   VkFormat depthFormat);

/// Превращает описание непрозрачного прохода в описание прохода воды.
void makeVoxelBlendDesc(vk::PipelineDesc& d);

} // namespace render

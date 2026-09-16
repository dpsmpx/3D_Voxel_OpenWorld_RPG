/**
 * @file grass_pipeline.h
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#pragma once
#include "../vk/vk_pipeline.h"
#include <glm/glm.hpp>

namespace render {

/// Вершина пучка травы: только положение, всё остальное — в инстансе.
struct GrassVertex { glm::vec3 p; };
static_assert(sizeof(GrassVertex) == 12, "привязка рассчитана на 12 байт");

/// Геометрия пучка и описание конвейера — общие для игры и для
/// офлайн-проверки графики (tools/vkcheck). Держать их порознь уже
/// однажды стоило дорого: проверка сверяла бы свою копию с собой.
const GrassVertex* grassVerts(u32& count);
const u32*         grassIndices(u32& count);

vk::PipelineDesc grassPipelineDesc(VkRenderPass rp, VkDescriptorSetLayout layout,
                                   VkFormat depthFormat);

} // namespace render

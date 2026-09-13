/**
 * @file vk_pipeline.h
 * @brief Тонкая обёртка над Vulkan: контекст, буферы, текстуры, пайплайны.
 */
#pragma once
#include "../core/types.h"
#include <vulkan/vulkan.h>

namespace vk {
class ShaderCache;

/// Описание вершинного формата. Раньше layout был зашит в
/// GraphicsPipeline одним вариантом (stride 24, vec3+vec2+rgba)
/// и совпадал только с воксельным шейдером — у UI, контура, мобов
/// и травы форматы другие. Теперь каждый рендерер объявляет свой.
struct VertexBinding {
    u32  stride      = 0;
    bool perInstance = false;
};

struct VertexAttr {
    u32      location = 0;
    u32      binding  = 0;
    VkFormat format   = VK_FORMAT_UNDEFINED;
    u32      offset   = 0;
};

struct PipelineDesc {
    VkRenderPass          renderPass;
    VkDescriptorSetLayout descLayout;
    const char*           vertName;
    const char*           fragName;
    VkFormat              depthFormat;

    /// Вершинный формат. Пустой список — рисование без вершинного
    /// буфера (например, полноэкранный треугольник скайбокса).
    const VertexBinding*  bindings     = nullptr;
    u32                   bindingCount = 0;
    const VertexAttr*     attrs        = nullptr;
    u32                   attrCount    = 0;
    VkCullModeFlags       cullMode   = VK_CULL_MODE_BACK_BIT;
    // Геометрия всюду намотана против часовой стрелки при взгляде
    // снаружи: и таблицы кубов (их стережёт check_winding.py), и
    // мешер чанков (его — проверка «обход граней смотрит наружу»).
    //
    // Здесь стояло VK_FRONT_FACE_CLOCKWISE с рассуждением, что
    // p[1][1] *= -1 в проекции переворачивает обход. Рассуждение
    // неверно ровно на один переворот: умножение на -1 меняет знак
    // NDC по Y, а видовое преобразование Vulkan с положительной
    // высотой переносит этот же знак в координаты кадра — второго
    // переворота не происходит. Наружная грань остаётся обходимой
    // против часовой стрелки, и лицевой её считает именно
    // COUNTER_CLOCKWISE.
    //
    // Цена ошибки была ровно та, которую описывает старый комментарий,
    // только с обратным знаком: отсекались наружные грани, и мир был
    // виден изнутри — сквозь землю светило небо, поверхности приходили
    // с открытостью неба 0 и потому чёрные, а вместо травы была
    // каменная изнанка. Проверено настоящим Vulkan на хосте
    // (tools/vkcheck): с CLOCKWISE картинка совпадает со снимком с
    // устройства, с COUNTER_CLOCKWISE — с эталонным программным
    // растеризатором.
    VkFrontFace           frontFace  = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool                  depthTest  = true;
    bool                  depthWrite = true;
    bool                  blend      = false;

    /// Phase 7: push constants
    u32                   pushConstantSize = 0;
    VkShaderStageFlags    pushConstantStage = VK_SHADER_STAGE_VERTEX_BIT;
};

class GraphicsPipeline {
public:
    bool create(VkDevice dev, ShaderCache& shaders, const PipelineDesc& d);
    void destroy();

    VkPipeline       handle() const { return pipeline_; }
    VkPipelineLayout layout() const { return layout_; }

    /// Создан ли пайплайн на самом деле. Часть рендереров движок
    /// заводит необязательными: не собрался — LOGW и живём дальше.
    /// Но нулевой дескриптор драйвер не проверяет, а разыменовывает:
    /// процесс падает внутри libvulkan, где от нашего кода не
    /// остаётся ни имени функции, ни строки. Спрашивать перед
    /// записью команд должен тот, кто их пишет.
    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    VkDevice         dev_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

} // namespace vk

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
    // снаружи — так принято и так её строит мешер. Но проекция камеры
    // переворачивает ось Y (p[1][1] *= -1, иначе картинка вверх ногами
    // в Vulkan), а вместе с ней меняется и направление обхода в
    // координатах кадра. Именно в них Vulkan и определяет лицевую
    // грань, поэтому здесь по часовой. С COUNTER_CLOCKWISE отсекались
    // ровно наружные грани, и мир был виден изнутри.
    VkFrontFace           frontFace  = VK_FRONT_FACE_CLOCKWISE;
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

private:
    VkDevice         dev_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

} // namespace vk

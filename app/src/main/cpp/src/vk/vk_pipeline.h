#pragma once
#include "../core/types.h"
#include <vulkan/vulkan.h>

namespace vk {
class ShaderCache;

struct PipelineDesc {
    VkRenderPass          renderPass;
    VkDescriptorSetLayout descLayout;
    const char*           vertName;
    const char*           fragName;
    VkFormat              depthFormat;
    VkCullModeFlags       cullMode   = VK_CULL_MODE_BACK_BIT;
    VkFrontFace           frontFace  = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool                  depthTest  = true;
    bool                  depthWrite = true;
    bool                  blend      = false;
    bool                  instanced  = false;

    // Phase 7: push constants
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
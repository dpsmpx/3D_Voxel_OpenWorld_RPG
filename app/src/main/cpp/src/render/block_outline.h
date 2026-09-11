#pragma once
#include "../core/types.h"
#include "../vk/vk_context.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_pipeline.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>

namespace render {

struct BlockOutlinePush {
    glm::vec4 pos;  // xyz = min-corner of block; w = 1.0 (unused)
};

class BlockOutline {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    void render(vk::Context& ctx, VkDescriptorSet uboSet,
                const glm::ivec3& blockPos, bool visible);

private:
    VkDevice             dev_ = VK_NULL_HANDLE;
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;
    vk::Buffer           vbo_;   // 24 vertices of a unit cube's edges
};

} // namespace render

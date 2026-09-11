#pragma once
#include "../core/types.h"
#include "../vk/vk_buffer.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_context.h"
#include "../ecs/registry.h"
#include "mob_renderer.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <vector>

namespace render {

class NpcRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    void rebuild(ecs::Registry& reg);
    void upload(vk::Context& ctx);
    void render(vk::Context& ctx);

    u32 instanceCount() const { return instanceCount_; }

private:
    VkDevice             dev_ = VK_NULL_HANDLE;
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;
    vk::Buffer           vbo_;
    vk::Buffer           ibo_;
    vk::Buffer           instanceGpu_;
    u64                  instanceCapacity_ = 0;

    std::vector<MobInstance> cpu_;
    u32 instanceCount_ = 0;
};

} // namespace render
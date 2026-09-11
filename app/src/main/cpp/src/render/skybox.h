#pragma once
#include "../core/types.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_context.h"
#include <android/asset_manager.h>

namespace render {

// ============================================================
// Skybox — fullscreen-triangle, рисуется ПЕРЕД сценой.
// Depth test/write выключены, поверхность заполняет весь экран,
// затем воксели перекрывают её.
// ============================================================
class Skybox {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();
    void render(vk::Context& ctx);

private:
    VkDevice              dev_ = VK_NULL_HANDLE;
    vk::ShaderCache       shaders_;
    vk::GraphicsPipeline  pipeline_;
};

} // namespace render
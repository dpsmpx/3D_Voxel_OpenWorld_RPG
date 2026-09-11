#include "skybox.h"
#include "../core/log.h"

namespace render {

bool Skybox::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
    shaders_.init(dev_, mgr);

    vk::PipelineDesc d{};
    d.renderPass   = ctx.renderPass();
    d.descLayout   = descLayout;
    d.vertName     = "shaders/sky.vert.spv";
    d.fragName     = "shaders/sky.frag.spv";
    d.depthFormat  = ctx.depthFormat();
    d.cullMode     = VK_CULL_MODE_NONE;
    d.depthTest    = false;
    d.depthWrite   = false;
    d.blend        = false;
    d.bindings     = nullptr;   // полноэкранный треугольник строится в шейдере
    d.bindingCount = 0;
    d.attrs        = nullptr;
    d.attrCount    = 0;

    if (!pipeline_.create(dev_, shaders_, d)) return false;
    LOGI("Skybox готов");
    return true;
}

void Skybox::render(vk::Context& ctx) {
    VkCommandBuffer cmd = ctx.currentCmd();

    VkViewport vp{};
    vp.width  = (f32)ctx.extent().width;
    vp.height = (f32)ctx.extent().height;
    vp.minDepth = 0.f; vp.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{}; sc.extent = ctx.extent();
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdDraw(cmd, 3, 1, 0, 0);   // fullscreen triangle
}

void Skybox::destroy() {
    pipeline_.destroy();
    shaders_.destroyAll();
}

} // namespace render

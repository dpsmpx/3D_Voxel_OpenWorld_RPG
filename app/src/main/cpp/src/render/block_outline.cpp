/**
 * @file block_outline.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "block_outline.h"
#include "../core/log.h"
#include <cstring>

namespace render {

// Вершинный формат контура: позиция + цвет, без UV.
static const vk::VertexBinding kBindings[1] = { { 28, false } };
static const vk::VertexAttr kAttrs[2] = {
    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0  },   // inPos
    { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12 },   // inColor
};

struct OutlineVertex {
    glm::vec3 pos;
    glm::vec4 color;
};

static const u32 EDGE[24][3] = {
    {0,0,0},{1,0,0}, {1,0,0},{1,0,1}, {1,0,1},{0,0,1}, {0,0,1},{0,0,0},
    {0,1,0},{1,1,0}, {1,1,0},{1,1,1}, {1,1,1},{0,1,1}, {0,1,1},{0,1,0},
    {0,0,0},{0,1,0}, {1,0,0},{1,1,0}, {1,0,1},{1,1,1}, {0,0,1},{0,1,1},
};

bool BlockOutline::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
    shaders_.init(dev_, mgr);

    OutlineVertex verts[24];
    for (int i = 0; i < 24; ++i) {
        verts[i].pos = glm::vec3(EDGE[i][0], EDGE[i][1], EDGE[i][2]);
        verts[i].color = glm::vec4(0.f, 0.f, 0.f, 1.f);
    }
    u64 bytes = sizeof(verts);
    if (!vbo_.create(dev_, ctx.physicalDevice(), bytes, vk::BufferUsage::Vertex, false)) return false;

    auto* s = new vk::Buffer();
    s->create(dev_, ctx.physicalDevice(), bytes, vk::BufferUsage::Staging, true);
    s->write(verts, bytes);
    ctx.submitOneShot([&](VkCommandBuffer cmd){
        VkBufferCopy c{0, 0, bytes};
        vkCmdCopyBuffer(cmd, s->handle(), vbo_.handle(), 1, &c);
    });
    s->destroy(); delete s;

    vk::PipelineDesc d{};
    d.renderPass  = ctx.renderPass();
    d.descLayout  = descLayout;
    d.vertName    = "shaders/outline.vert.spv";
    d.fragName    = "shaders/outline.frag.spv";
    d.depthFormat = ctx.depthFormat();
    d.cullMode    = VK_CULL_MODE_NONE;
    d.depthTest   = true;
    d.depthWrite  = false;
    d.blend       = true;
    d.bindings     = kBindings;
    d.bindingCount = 1;
    d.attrs        = kAttrs;
    d.attrCount    = 2;
    d.pushConstantSize  = sizeof(BlockOutlinePush);
    d.pushConstantStage = VK_SHADER_STAGE_VERTEX_BIT;
    if (!pipeline_.create(dev_, shaders_, d)) return false;

    LOGI("BlockOutline готов (push constants)");
    return true;
}

void BlockOutline::render(vk::Context& ctx, VkDescriptorSet uboSet,
                          const glm::ivec3& bp, bool visible)
{
    if (!visible) return;
    VkCommandBuffer cmd = ctx.currentCmd();

    // Лёгкое расширение наружу — чтобы линии не сливались с блоками
    BlockOutlinePush pc{};
    pc.pos = glm::vec4((f32)bp.x - 0.002f, (f32)bp.y - 0.002f,
                       (f32)bp.z - 0.002f, 1.f);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_.layout(), 0, 1, &uboSet, 0, nullptr);
    vkCmdPushConstants(cmd, pipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

    VkDeviceSize offs[] = { 0 };
    const VkBuffer vb = vbo_.handle();
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, offs);
    vkCmdDraw(cmd, 24, 1, 0, 0);
}

void BlockOutline::destroy() {
    vbo_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
}

} // namespace render

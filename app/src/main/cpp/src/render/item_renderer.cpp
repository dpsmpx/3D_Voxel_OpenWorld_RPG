#include "item_renderer.h"
#include "../items/item_pickup.h"
#include "../items/item_def.h"
#include "../ecs/components.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>

namespace render {

namespace {

struct CubeVertex { glm::vec3 pos; };
constexpr CubeVertex CUBE_V[24] = {
    {{-0.5f,-0.5f,-0.5f}},{{ 0.5f,-0.5f,-0.5f}},{{ 0.5f, 0.5f,-0.5f}},{{-0.5f, 0.5f,-0.5f}},
    {{-0.5f,-0.5f, 0.5f}},{{ 0.5f,-0.5f, 0.5f}},{{ 0.5f, 0.5f, 0.5f}},{{-0.5f, 0.5f, 0.5f}},
    {{-0.5f,-0.5f,-0.5f}},{{-0.5f, 0.5f,-0.5f}},{{-0.5f, 0.5f, 0.5f}},{{-0.5f,-0.5f, 0.5f}},
    {{ 0.5f,-0.5f,-0.5f}},{{ 0.5f, 0.5f,-0.5f}},{{ 0.5f, 0.5f, 0.5f}},{{ 0.5f,-0.5f, 0.5f}},
    {{-0.5f,-0.5f,-0.5f}},{{ 0.5f,-0.5f,-0.5f}},{{ 0.5f,-0.5f, 0.5f}},{{-0.5f,-0.5f, 0.5f}},
    {{-0.5f, 0.5f,-0.5f}},{{ 0.5f, 0.5f,-0.5f}},{{ 0.5f, 0.5f, 0.5f}},{{-0.5f, 0.5f, 0.5f}},
};
constexpr u32 CUBE_I[36] = {
     0, 1, 2,  0, 2, 3,
     4, 5, 6,  4, 6, 7,
     8, 9,10,  8,10,11,
    12,13,14, 12,14,15,
    16,17,18, 16,18,19,
    20,21,22, 20,22,23,
};

// Цвет по редкости предмета
u32 colorForItem(u16 itemId) {
    const auto& def = items::items().get(itemId);
    switch (def.rarity) {
        case items::ItemRarity::Common:    return 0xCCCCCCFF;
        case items::ItemRarity::Uncommon:  return 0x60D060FF;
        case items::ItemRarity::Rare:      return 0x4090FFFF;
        case items::ItemRarity::Epic:      return 0xB060FFFF;
        case items::ItemRarity::Legendary: return 0xFFB040FF;
        default:                           return 0xFFFFFFFF;
    }
}

} // namespace

bool ItemRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
    shaders_.init(dev_, mgr);

    u64 vbBytes = sizeof(CUBE_V);
    if (!vbo_.create(dev_, ctx.physicalDevice(), vbBytes, vk::BufferUsage::Vertex, false)) return false;
    {
        auto* s = new vk::Buffer();
        s->create(dev_, ctx.physicalDevice(), vbBytes, vk::BufferUsage::Staging, true);
        s->write(CUBE_V, vbBytes);
        ctx.submitOneShot([&](VkCommandBuffer cmd){
            VkBufferCopy c{0,0,vbBytes};
            vkCmdCopyBuffer(cmd, s->handle(), vbo_.handle(), 1, &c);
        });
        s->destroy(); delete s;
    }

    u64 ibBytes = sizeof(CUBE_I);
    if (!ibo_.create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Index, false)) return false;
    {
        auto* s = new vk::Buffer();
        s->create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Staging, true);
        s->write(CUBE_I, ibBytes);
        ctx.submitOneShot([&](VkCommandBuffer cmd){
            VkBufferCopy c{0,0,ibBytes};
            vkCmdCopyBuffer(cmd, s->handle(), ibo_.handle(), 1, &c);
        });
        s->destroy(); delete s;
    }

    vk::PipelineDesc d{};
    d.renderPass  = ctx.renderPass();
    d.descLayout  = descLayout;
    d.vertName    = "shaders/projectile.vert.spv";
    d.fragName    = "shaders/projectile.frag.spv";
    d.depthFormat = ctx.depthFormat();
    d.cullMode    = VK_CULL_MODE_BACK_BIT;
    d.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    d.depthTest   = true;
    d.depthWrite  = true;
    d.blend       = false;
    d.instanced   = true;
    if (!pipeline_.create(dev_, shaders_, d)) return false;

    cpu_.reserve(256);
    LOGI("ItemRenderer готов");
    return true;
}

void ItemRenderer::rebuild(ecs::Registry& reg) {
    cpu_.clear();

    auto& pool = reg.pool<items::ItemPickup>();
    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* p  = pool.get(e);
        auto* tf = reg.get<ecs::Transform>(e);
        if (!p || !tf) continue;
        if (p->stack.empty()) continue;

        // Мигание при исчезновении
        f32 alpha = 1.f;
        if (p->lifeRemaining < 5.f) {
            f32 blink = std::sin(p->blinkTimer * 3.14159f);
            if (blink < 0.f) alpha = 0.2f;
        }

        u32 c = colorForItem(p->stack.itemId);
        if (alpha < 1.f) {
            u8 a = (u8)(((c      ) & 0xFF) * alpha);
            c = (c & 0xFFFFFF00u) | a;
        }

        // Покачивание в воздухе
        f32 bob = std::sin(tf->position.x * 3.0f + tf->position.z * 2.0f +
                           p->lifeRemaining * 0.5f) * 0.08f;

        MobInstance inst{};
        inst.pos   = tf->position + glm::vec3(0, 0.25f + bob, 0);
        inst.size  = glm::vec3(0.35f);
        inst.color = c;
        inst.yaw   = tf->position.x * 0.3f + tf->position.z * 0.4f;

        cpu_.push_back(inst);
    }

    instanceCount_ = (u32)cpu_.size();
}

void ItemRenderer::upload(vk::Context& ctx) {
    if (instanceCount_ == 0) return;

    u64 bytes = (u64)instanceCount_ * sizeof(MobInstance);
    if (!instanceGpu_.handle() || instanceCapacity_ < bytes) {
        if (instanceGpu_.handle()) instanceGpu_.destroy();
        if (!instanceGpu_.create(dev_, ctx.physicalDevice(), bytes,
                                 vk::BufferUsage::Vertex, false)) return;
        instanceCapacity_ = bytes;
    }

    auto* s = new vk::Buffer();
    s->create(dev_, ctx.physicalDevice(), bytes, vk::BufferUsage::Staging, true);
    s->write(cpu_.data(), bytes);
    ctx.submitOneShot([&](VkCommandBuffer cmd){
        VkBufferCopy c{0,0,bytes};
        vkCmdCopyBuffer(cmd, s->handle(), instanceGpu_.handle(), 1, &c);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    });
    s->destroy(); delete s;
}

void ItemRenderer::render(vk::Context& ctx) {
    if (instanceCount_ == 0 || !instanceGpu_.handle()) return;
    VkCommandBuffer cmd = ctx.currentCmd();

    VkViewport vp{};
    vp.width  = (f32)ctx.extent().width;
    vp.height = (f32)ctx.extent().height;
    vp.minDepth = 0.f; vp.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{}; sc.extent = ctx.extent();
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    VkBuffer vbs[2] = { vbo_.handle(), instanceGpu_.handle() };
    VkDeviceSize offs[2] = { 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
    vkCmdBindIndexBuffer(cmd, ibo_.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, 36, instanceCount_, 0, 0, 0);
}

void ItemRenderer::destroy() {
    vbo_.destroy();
    ibo_.destroy();
    instanceGpu_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render
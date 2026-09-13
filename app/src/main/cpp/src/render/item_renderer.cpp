/**
 * @file item_renderer.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "item_renderer.h"
#include "../items/item_pickup.h"
#include "../items/item_def.h"
#include "../ecs/components.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>

namespace render {

// Вершинный формат: единичный куб (vec3) + инстанс
// pos/size/color/yaw, ровно как в MobInstance.
static const vk::VertexBinding kBindings[2] = {
    { 12,                      false },   // CubeVertex: glm::vec3
    { sizeof(MobInstance),     true  },
};
static const vk::VertexAttr kAttrs[5] = {
    { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0  },   // inPos
    { 1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0  },   // iPos
    { 2, 1, VK_FORMAT_R32G32B32_SFLOAT, 12 },   // iSize
    { 3, 1, VK_FORMAT_R8G8B8A8_UNORM,   24 },   // iColor
    { 4, 1, VK_FORMAT_R32_SFLOAT,       28 },   // iYaw
};

namespace {

struct CubeVertex { glm::vec3 pos; };
static_assert(sizeof(CubeVertex) == 12, "kBindings рассчитан на 12 байт");
constexpr CubeVertex CUBE_V[24] = {
    {{-0.5f,-0.5f,-0.5f}},{{ 0.5f,-0.5f,-0.5f}},{{ 0.5f, 0.5f,-0.5f}},{{-0.5f, 0.5f,-0.5f}},
    {{-0.5f,-0.5f, 0.5f}},{{ 0.5f,-0.5f, 0.5f}},{{ 0.5f, 0.5f, 0.5f}},{{-0.5f, 0.5f, 0.5f}},
    {{-0.5f,-0.5f,-0.5f}},{{-0.5f, 0.5f,-0.5f}},{{-0.5f, 0.5f, 0.5f}},{{-0.5f,-0.5f, 0.5f}},
    {{ 0.5f,-0.5f,-0.5f}},{{ 0.5f, 0.5f,-0.5f}},{{ 0.5f, 0.5f, 0.5f}},{{ 0.5f,-0.5f, 0.5f}},
    {{-0.5f,-0.5f,-0.5f}},{{ 0.5f,-0.5f,-0.5f}},{{ 0.5f,-0.5f, 0.5f}},{{-0.5f,-0.5f, 0.5f}},
    {{-0.5f, 0.5f,-0.5f}},{{ 0.5f, 0.5f,-0.5f}},{{ 0.5f, 0.5f, 0.5f}},{{-0.5f, 0.5f, 0.5f}},
};
constexpr u32 CUBE_I[36] = {
    // Все грани обходятся против часовой стрелки при взгляде СНАРУЖИ.
    // Раньше -Z, -X и +Y были намотаны наоборот, и при отсечении
    // задних граней половина каждого куба просвечивала насквозь.
     0, 2, 1,   0, 3, 2,     // -Z
     4, 5, 6,   4, 6, 7,     // +Z
     8,10, 9,   8,11,10,     // -X
    12,13,14,  12,14,15,     // +X
    16,17,18,  16,18,19,     // -Y
    20,22,21,  20,23,22,     // +Y
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
    instances_.init(dev_, ctx.physicalDevice());
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
    // Выпавшие предметы — обычные кубы, а не свечение: им нужно то же
    // освещение, что мобам, иначе они горят ровным цветом даже ночью.
    d.vertName    = "shaders/mob.vert.spv";
    d.fragName    = "shaders/mob.frag.spv";
    d.depthFormat = ctx.depthFormat();
    d.cullMode    = VK_CULL_MODE_BACK_BIT;
    d.depthTest   = true;
    d.depthWrite  = true;
    d.blend       = false;
    d.bindings     = kBindings;
    d.bindingCount = 2;
    d.attrs        = kAttrs;
    d.attrCount    = 5;
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
    (void)ctx;
    instanceCount_ = (u32)cpu_.size();
    if (instanceCount_ == 0) return;
    // Пишем прямо в память, видимую процессору: ни временного буфера,
    // ни отдельной отправки в очередь, ни ожидания GPU.
    if (!instances_.write(cpu_.data(),
                          (u64)instanceCount_ * sizeof(MobInstance)))
        instanceCount_ = 0;
}

void ItemRenderer::render(vk::Context& ctx, VkDescriptorSet set) {
    if (instanceCount_ == 0 || !instances_.handle() || !pipeline_.valid()) return;
    VkCommandBuffer cmd = ctx.currentCmd();

    VkViewport vp{};
    vp.width  = (f32)ctx.extent().width;
    vp.height = (f32)ctx.extent().height;
    vp.minDepth = 0.f; vp.maxDepth = 1.f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{}; sc.extent = ctx.extent();
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_.layout(), 0, 1, &set, 0, nullptr);
    VkBuffer vbs[2] = { vbo_.handle(), instances_.handle() };
    VkDeviceSize offs[2] = { 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
    vkCmdBindIndexBuffer(cmd, ibo_.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, 36, instanceCount_, 0, 0, 0);
}

void ItemRenderer::destroy() {
    vbo_.destroy();
    ibo_.destroy();
    instances_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render

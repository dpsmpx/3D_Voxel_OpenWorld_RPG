/**
 * @file mob_renderer.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "mob_renderer.h"
#include "../mobs/mob_def.h"
#include "../mobs/mob_ai.h"
#include "../ecs/components.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>
#include <algorithm>

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

// Единичный куб: 24 вершины (pos + цвет задаётся инстансом)
struct CubeVertex { glm::vec3 pos; };
static_assert(sizeof(CubeVertex) == 12, "kBindings рассчитан на 12 байт");
constexpr CubeVertex CUBE_V[24] = {
    // -Z
    {{-0.5f,-0.5f,-0.5f}},{{ 0.5f,-0.5f,-0.5f}},{{ 0.5f, 0.5f,-0.5f}},{{-0.5f, 0.5f,-0.5f}},
    // +Z
    {{-0.5f,-0.5f, 0.5f}},{{ 0.5f,-0.5f, 0.5f}},{{ 0.5f, 0.5f, 0.5f}},{{-0.5f, 0.5f, 0.5f}},
    // -X
    {{-0.5f,-0.5f,-0.5f}},{{-0.5f, 0.5f,-0.5f}},{{-0.5f, 0.5f, 0.5f}},{{-0.5f,-0.5f, 0.5f}},
    // +X
    {{ 0.5f,-0.5f,-0.5f}},{{ 0.5f, 0.5f,-0.5f}},{{ 0.5f, 0.5f, 0.5f}},{{ 0.5f,-0.5f, 0.5f}},
    // -Y
    {{-0.5f,-0.5f,-0.5f}},{{ 0.5f,-0.5f,-0.5f}},{{ 0.5f,-0.5f, 0.5f}},{{-0.5f,-0.5f, 0.5f}},
    // +Y
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

// Применить анимацию к offset части
glm::vec3 animOffset(const mobs::MobPart& part, const mobs::MobAI& ai,
                     f32 speedNorm, bool attacking, bool dying)
{
    glm::vec3 off = part.offset;
    const f32 phase = ai.walkPhase;

    switch (part.anim) {
        case mobs::PartAnim::Leg:
        case mobs::PartAnim::LegOpp: {
            f32 mult = (part.anim == mobs::PartAnim::Leg) ? 1.f : -1.f;
            f32 swing = std::sin(phase) * 0.15f * speedNorm * mult;
            off.z += swing;
            break;
        }
        case mobs::PartAnim::Arm:
        case mobs::PartAnim::ArmOpp: {
            f32 mult = (part.anim == mobs::PartAnim::Arm) ? 1.f : -1.f;
            f32 swing = std::sin(phase) * 0.12f * speedNorm * mult;
            off.z += swing;
            // При атаке — рука вперёд
            if (attacking) off.z += ai.attackAnim * 0.35f;
            break;
        }
        case mobs::PartAnim::Head:
            off.y += std::sin(phase * 2.f) * 0.02f * speedNorm;
            break;
        case mobs::PartAnim::Tail:
            off.x += std::sin(phase * 1.5f) * 0.06f;
            break;
        default: break;
    }

    // Смерть: слегка утапливается
    if (dying) off.y -= 0.25f * (ai.deathTimer / 1.6f);

    return off;
}

} // namespace

bool MobRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
    shaders_.init(dev_, mgr);

    // VBO
    u64 vbBytes = sizeof(CUBE_V);
    if (!vbo_.create(dev_, ctx.physicalDevice(), vbBytes, vk::BufferUsage::Vertex, false)) return false;
    auto* svb = new vk::Buffer();
    svb->create(dev_, ctx.physicalDevice(), vbBytes, vk::BufferUsage::Staging, true);
    svb->write(CUBE_V, vbBytes);
    ctx.submitOneShot([&](VkCommandBuffer cmd){
        VkBufferCopy c{0,0,vbBytes};
        vkCmdCopyBuffer(cmd, svb->handle(), vbo_.handle(), 1, &c);
    });
    svb->destroy(); delete svb;

    // IBO
    u64 ibBytes = sizeof(CUBE_I);
    if (!ibo_.create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Index, false)) return false;
    auto* sib = new vk::Buffer();
    sib->create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Staging, true);
    sib->write(CUBE_I, ibBytes);
    ctx.submitOneShot([&](VkCommandBuffer cmd){
        VkBufferCopy c{0,0,ibBytes};
        vkCmdCopyBuffer(cmd, sib->handle(), ibo_.handle(), 1, &c);
    });
    sib->destroy(); delete sib;

    vk::PipelineDesc d{};
    d.renderPass  = ctx.renderPass();
    d.descLayout  = descLayout;
    d.vertName    = "shaders/mob.vert.spv";
    d.fragName    = "shaders/mob.frag.spv";
    d.depthFormat = ctx.depthFormat();
    d.cullMode    = VK_CULL_MODE_BACK_BIT;
    d.frontFace   = VK_FRONT_FACE_CLOCKWISE;   // см. PipelineDesc: Y-flip проекции
    d.depthTest   = true;
    d.depthWrite  = true;
    d.blend       = false;
    d.bindings     = kBindings;
    d.bindingCount = 2;
    d.attrs        = kAttrs;
    d.attrCount    = 5;
    if (!pipeline_.create(dev_, shaders_, d)) return false;

    LOGI("MobRenderer готов");
    return true;
}

void MobRenderer::rebuild(ecs::Registry& reg) {
    cpuInstances_.clear();

    auto& pool = reg.pool<mobs::MobAI>();
    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* ai   = pool.get(e);
        auto* tf   = reg.get<ecs::Transform>(e);
        auto* tag  = reg.get<mobs::MobTag>(e);
        auto* vel  = reg.get<ecs::Velocity>(e);
        auto* hp   = reg.get<ecs::Health>(e);
        if (!ai || !tf || !tag || !vel || !hp) continue;

        const mobs::MobDef& def = mobs::mobRegistry().get(tag->id);
        if (tag->id == mobs::MOB_NONE) continue;

        // Ориентация: смотрим вдоль velocity (XZ)
        glm::vec2 velXZ { vel->linear.x, vel->linear.z };
        f32 yaw = 0.f;
        if (glm::length(velXZ) > 0.1f) {
            yaw = std::atan2(velXZ.x, velXZ.y);   // 0 = +Z
        }

        const f32 speedNorm = glm::min(1.f, glm::length(velXZ) / std::max(0.1f, def.chaseSpeed));
        const bool attacking = (ai->attackAnim > 0.01f);
        const bool dying = (ai->deathTimer > 0.f);

        for (u8 p = 0; p < def.partCount; ++p) {
            const mobs::MobPart& part = def.parts[p];
            if (part.size.x <= 0.f || part.size.y <= 0.f || part.size.z <= 0.f) continue;

            glm::vec3 off = animOffset(part, *ai, speedNorm, attacking, dying);

            MobInstance inst{};
            inst.pos   = tf->position + glm::vec3(0, off.y, 0);
            // offset x/z — с учётом yaw
            f32 c = std::cos(yaw), s = std::sin(yaw);
            inst.pos.x += off.x * c - off.z * s;
            inst.pos.z += off.x * s + off.z * c;
            inst.size  = part.size;
            inst.color = part.color;
            inst.yaw   = yaw;

            // Красная вспышка при получении урона
            if (ai->damageFlash > 0.f) {
                u8 r = (inst.color >> 24) & 0xFF;
                u8 g = (inst.color >> 16) & 0xFF;
                u8 b = (inst.color >>  8) & 0xFF;
                f32 t = ai->damageFlash / 0.25f;
                r = (u8)(r * (1.f - t) + 255.f * t);
                g = (u8)(g * (1.f - t) +  40.f * t);
                b = (u8)(b * (1.f - t) +  40.f * t);
                inst.color = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFF;
            }

            cpuInstances_.push_back(inst);
        }
    }

    instanceCount_ = (u32)cpuInstances_.size();
}

void MobRenderer::upload(vk::Context& ctx) {
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
    s->write(cpuInstances_.data(), bytes);
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

void MobRenderer::render(vk::Context& ctx, const math::Frustum&) {
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

void MobRenderer::destroy() {
    vbo_.destroy(); ibo_.destroy(); instanceGpu_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
}

} // namespace render

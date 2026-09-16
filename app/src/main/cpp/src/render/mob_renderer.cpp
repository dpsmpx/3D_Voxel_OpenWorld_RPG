/**
 * @file mob_renderer.cpp
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#include "mob_renderer.h"
#include "../mobs/mob_def.h"
#include "../mobs/mob_ai.h"
#include "../ecs/components.h"
#include "../core/orientation.h"
#include "../entity/rig.h"
#include "../entity/mob_rigs.h"
#include "../entity/locomotion.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>
#include <algorithm>

namespace render {

// Единичный куб: 24 вершины, по четыре на грань, в порядке
// -Z, +Z, -X, +X, -Y, +Y. Этот порядок знает mob.vert — он берёт из
// него нормаль как gl_VertexIndex / 4. Раньше таблица лежала в
// анонимном пространстве имён каждого из четырёх рендеров.
const CubeVertex MOB_CUBE_V[24] = {
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
// Все грани обходятся против часовой стрелки при взгляде СНАРУЖИ.
// Раньше -Z, -X и +Y были намотаны наоборот, и при отсечении
// задних граней половина каждого куба просвечивала насквозь.
const u32 MOB_CUBE_I[36] = {
     0, 2, 1,   0, 3, 2,     // -Z
     4, 5, 6,   4, 6, 7,     // +Z
     8,10, 9,   8,11,10,     // -X
    12,13,14,  12,14,15,     // +X
    16,17,18,  16,18,19,     // -Y
    20,22,21,  20,23,22,     // +Y
};

bool MobRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
    instances_.init(dev_, ctx.physicalDevice());
    shaders_.init(dev_, mgr);

    // VBO
    u64 vbBytes = sizeof(MOB_CUBE_V);
    if (!vbo_.create(dev_, ctx.physicalDevice(), vbBytes, vk::BufferUsage::Vertex, false)) return false;
    auto* svb = new vk::Buffer();
    svb->create(dev_, ctx.physicalDevice(), vbBytes, vk::BufferUsage::Staging, true);
    svb->write(MOB_CUBE_V, vbBytes);
    ctx.submitOneShot([&](VkCommandBuffer cmd){
        VkBufferCopy c{0,0,vbBytes};
        vkCmdCopyBuffer(cmd, svb->handle(), vbo_.handle(), 1, &c);
    });
    svb->destroy(); delete svb;

    // IBO
    u64 ibBytes = sizeof(MOB_CUBE_I);
    if (!ibo_.create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Index, false)) return false;
    auto* sib = new vk::Buffer();
    sib->create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Staging, true);
    sib->write(MOB_CUBE_I, ibBytes);
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
    d.depthTest   = true;
    d.depthWrite  = true;
    d.blend       = false;
    d.bindings     = MOB_BINDINGS;
    d.bindingCount = MOB_BINDING_COUNT;
    d.attrs        = MOB_ATTRS;
    d.attrCount    = MOB_ATTR_COUNT;
    if (!pipeline_.create(dev_, shaders_, d)) return false;

    LOGI("MobRenderer готов");
    return true;
}

void MobRenderer::rebuild(ecs::Registry& reg, f32 timeSec) {
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
        // Поворот БЕРЁТСЯ из состояния сущности, а не вычисляется
        // здесь. Прежний код считал его из мгновенной скорости в
        // локальную переменную и обнулял при остановке — отсюда
        // мгновенный разворот на север, стоило существу встать.
        const glm::vec2 velXZ { vel->linear.x, vel->linear.z };
        const auto* fc = reg.get<ecs::Facing>(e);
        const f32 yaw = fc ? fc->yaw : orient::yawFromDirection(velXZ.x, velXZ.y);

        const f32 speedNorm = glm::min(1.f, glm::length(velXZ) / std::max(0.1f, def.chaseSpeed));
        const bool attacking = (ai->attackAnim > 0.01f);
        const bool dying = (ai->deathTimer > 0.f);

        // Оснастка вместо плоского списка коробок.
        //
        // Определение вида даёт иерархию, анимация — позу, а сборка
        // в мировые коробки живёт в entity::resolve. Рендер больше не
        // знает, что такое «нога»: он ставит то, что ему дали.
        const entity::Rig& rig = mobs::rigFor(tag->id);

        anim::AnimState st;
        // Фаза шага — состояние сущности, посчитанное ПУТЁМ, а не
        // множителем ко времени в ИИ.
        const auto* gt = reg.get<ecs::Gait>(e);
        st.phase     = gt ? gt->phase : 0.f;
        st.time      = timeSec;

        // Прыжок, падение и приземление — состояние сущности, не
        // догадка рендера по вертикальной скорости.
        if (const auto* lo = reg.get<ecs::Locomotion>(e)) {
            st.air  = lo->air;
            st.land = lo->land;
            st.rise = lo->rise;
        }
        st.speedNorm = speedNorm;
        st.attack    = attacking ? ai->attackAnim : 0.f;
        st.death     = dying ? ai->deathTimer : 0.f;

        entity::Pose pose;
        anim::poseFor(rig, pose, st);

        entity::ResolvedPart parts[entity::MAX_PARTS];
        const u8 n = entity::resolve(rig, pose, tf->position, yaw,
                                     parts, entity::MAX_PARTS);

        for (u8 p = 0; p < n; ++p) {
            MobInstance inst{};
            inst.pos   = parts[p].center;
            inst.size  = parts[p].size;
            inst.color = parts[p].color;
            inst.rot   = glm::vec4(parts[p].rot.x, parts[p].rot.y,
                                   parts[p].rot.z, parts[p].rot.w);

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
    (void)ctx;
    instanceCount_ = (u32)cpuInstances_.size();
    if (instanceCount_ == 0) return;
    // Пишем прямо в память, видимую процессору: ни временного буфера,
    // ни отдельной отправки в очередь, ни ожидания GPU.
    if (!instances_.write(cpuInstances_.data(),
                          (u64)instanceCount_ * sizeof(MobInstance)))
        instanceCount_ = 0;
}

void MobRenderer::render(vk::Context& ctx, VkDescriptorSet set, const math::Frustum&) {
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

void MobRenderer::destroy() {
    vbo_.destroy(); ibo_.destroy(); instances_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
}

} // namespace render

/**
 * @file npc_renderer.cpp
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#include "npc_renderer.h"
#include "../npc/npc_def.h"
#include "../npc/npc_ai.h"
#include "../ecs/components.h"
#include "../core/orientation.h"
#include "../entity/rig.h"
#include "../npc/npc_rig.h"
#include "../player/player_rig.h"
#include "../combat/components.h"
#include "../entity/locomotion.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>
#include <algorithm>

namespace render {

// Вершинный формат и геометрия куба — общие для всех рендеров
// MobInstance, см. MOB_ATTRS в mob_renderer.h.


bool NpcRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
    dev_ = ctx.device();
    instances_.init(dev_, ctx.physicalDevice());
    shaders_.init(dev_, mgr);

    u64 vbBytes = sizeof(MOB_CUBE_V);
    if (!vbo_.create(dev_, ctx.physicalDevice(), vbBytes, vk::BufferUsage::Vertex, false)) return false;
    {
        auto* s = new vk::Buffer();
        s->create(dev_, ctx.physicalDevice(), vbBytes, vk::BufferUsage::Staging, true);
        s->write(MOB_CUBE_V, vbBytes);
        ctx.submitOneShot([&](VkCommandBuffer cmd){
            VkBufferCopy c{0,0,vbBytes};
            vkCmdCopyBuffer(cmd, s->handle(), vbo_.handle(), 1, &c);
        });
        s->destroy(); delete s;
    }

    u64 ibBytes = sizeof(MOB_CUBE_I);
    if (!ibo_.create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Index, false)) return false;
    {
        auto* s = new vk::Buffer();
        s->create(dev_, ctx.physicalDevice(), ibBytes, vk::BufferUsage::Staging, true);
        s->write(MOB_CUBE_I, ibBytes);
        ctx.submitOneShot([&](VkCommandBuffer cmd){
            VkBufferCopy c{0,0,ibBytes};
            vkCmdCopyBuffer(cmd, s->handle(), ibo_.handle(), 1, &c);
        });
        s->destroy(); delete s;
    }

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

    cpu_.reserve(128);
    LOGI("NpcRenderer готов");
    return true;
}

void NpcRenderer::rebuild(ecs::Registry& reg, f32 timeSec, bool showPlayer) {
    cpu_.clear();

    auto& pool = reg.pool<npc::NpcAI>();
    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* ai   = pool.get(e);
        auto* tf   = reg.get<ecs::Transform>(e);
        auto* tag  = reg.get<npc::NpcTag>(e);
        auto* vel  = reg.get<ecs::Velocity>(e);
        auto* hp   = reg.get<ecs::Health>(e);
        if (!ai || !tf || !tag || !vel || !hp) continue;

        const npc::NpcDef& def = npc::npcRegistry().get(tag->id);
        if (tag->id == npc::NPC_NONE) continue;

        // Ориентация
        glm::vec2 velXZ { vel->linear.x, vel->linear.z };
        // Поворот БЕРЁТСЯ из состояния сущности, а не вычисляется
        // здесь. Прежний код считал его из мгновенной скорости в
        // локальную переменную и обнулял при остановке — отсюда
        // мгновенный разворот на север, стоило NPC встать.
        const auto* fc = reg.get<ecs::Facing>(e);
        const f32 yaw = fc ? fc->yaw : orient::yawFromDirection(velXZ.x, velXZ.y);
        const f32 speedNorm = glm::min(1.f, glm::length(velXZ) /
                                              std::max(0.1f, def.moveSpeed));

        // ---- Оснастка ----
        //
        // Раньше здесь вручную собирались шесть коробок, и ноги
        // «шагали» сдвигом по Z на sin(walkPhase) — параллельно себе,
        // без сустава. Заодно коробка тела стояла центром в точке
        // опоры (наполовину под землёй), а голова висела на 1.45 — с
        // просветом там, где полагалась грудь. Теперь геометрию даёт
        // оснастка, позу — общая локомоция, сборку — entity::resolve.
        // Облик особи, а не вида: шесть селян в деревне обязаны
        // отличаться друг от друга не только координатами.
        const auto* look = reg.get<ecs::Appearance>(e);
        const entity::Rig& rig = npc::rigFor(tag->id, look ? look->seed : 0u);

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
        st.death     = (ai->state == npc::NpcAI::Dead) ? ai->deathTimer : 0.f;

        entity::Pose pose;
        anim::poseFor(rig, pose, st);

        // Смерть: тело ещё и оседает — заваливание задаёт поза.
        const f32 deathSink = (ai->state == npc::NpcAI::Dead)
                            ? std::min(0.8f, ai->deathTimer * 0.5f) : 0.f;
        const glm::vec3 root = tf->position - glm::vec3(0.f, deathSink, 0.f);

        entity::ResolvedPart parts[entity::MAX_PARTS];
        const u8 n = entity::resolve(rig, pose, root, yaw,
                                     parts, entity::MAX_PARTS);

        for (u8 k = 0; k < n; ++k) {
            MobInstance inst{};
            inst.pos   = parts[k].center;
            inst.size  = parts[k].size;
            inst.rot   = glm::vec4(parts[k].rot.x, parts[k].rot.y,
                                   parts[k].rot.z, parts[k].rot.w);
            u32 rgba   = parts[k].color;

            if (ai->damageFlash > 0.f) {
                const f32 t = ai->damageFlash / 0.15f;
                u8 r = (rgba >> 24) & 0xFF;
                u8 g = (rgba >> 16) & 0xFF;
                u8 b = (rgba >>  8) & 0xFF;
                r = (u8)(r * (1.f - t) + 255.f * t);
                g = (u8)(g * (1.f - t) +  40.f * t);
                b = (u8)(b * (1.f - t) +  40.f * t);
                rgba = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFF;
            }
            inst.colorGpu = packInstanceColor(rgba);
            cpu_.push_back(inst);
        }

        // ---- Значок над головой ----
        //
        // Не часть тела: он не поворачивается вместе с NPC и не
        // участвует в анимации, поэтому в оснастке ему не место.
        if (ai->state != npc::NpcAI::Dead) {
            const bool quest  = (def.role == npc::NpcRole::QuestGiver);
            const bool trader = (def.role == npc::NpcRole::Trader);
            if (quest || trader) {
                MobInstance inst{};
                inst.pos   = tf->position +
                             glm::vec3(0.f, def.bodyHeight + 0.35f, 0.f);
                inst.size  = quest ? glm::vec3(0.35f, 0.35f, 0.1f)
                                   : glm::vec3(0.30f, 0.30f, 0.1f);
                inst.colorGpu = packInstanceColor(
                                     quest ? 0xFFD040FFu    // жёлтый «!»
                                           : 0xFFC040FFu);  // золотая монета
                inst.rot   = orient::yawQuat(yaw);
                cpu_.push_back(inst);
            }
        }
    }

    if (showPlayer) appendPlayer(reg, timeSec);

    instanceCount_ = (u32)cpu_.size();
}

// Игрок рисуется ЗДЕСЬ, а не отдельным рендером.
//
// Он такой же двуногий, как селянин, и формат инстанса у него тот же,
// поэтому свой конвейер, свой буфер и свой вызов отрисовки ему не
// нужны — это была бы лишняя цена за отсутствующее отличие. Раньше
// игрока не рисовали вовсе: камера стоит в пяти с половиной метрах
// позади, а показывать там было нечего.
void NpcRenderer::appendPlayer(ecs::Registry& reg, f32 timeSec) {
    auto& pool = reg.pool<ecs::PlayerTag>();
    for (usize i = 0; i < pool.size(); ++i) {
        const ecs::Entity e = pool.entityAt((u32)i);
        const auto* tf = reg.get<ecs::Transform>(e);
        const auto* vel = reg.get<ecs::Velocity>(e);
        if (!tf || !vel) continue;

        const auto* fc = reg.get<ecs::Facing>(e);
        const auto* gt = reg.get<ecs::Gait>(e);
        const auto* ws = reg.get<combat::WeaponState>(e);

        const glm::vec2 velXZ { vel->linear.x, vel->linear.z };
        const f32 yaw = fc ? fc->yaw : orient::yawFromDirection(velXZ.x, velXZ.y);

        // Нормируем по скорости бега: на спринте шаг обязан быть
        // шире, чем на шаге, — иначе обе скорости выглядят одинаково.
        const f32 speedNorm = glm::min(1.f, glm::length(velXZ) / 7.5f);

        const entity::Rig& rg = player::rig();

        anim::AnimState st;
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
        st.attack    = ws ? glm::clamp(ws->swingAnim, 0.f, 1.f) : 0.f;
        if (const auto* lo = reg.get<ecs::Locomotion>(e)) {
            st.air  = lo->air;
            st.land = lo->land;
            st.rise = lo->rise;
        }

        entity::Pose pose;
        anim::poseFor(rg, pose, st);

        entity::ResolvedPart parts[entity::MAX_PARTS];
        const u8 n = entity::resolve(rg, pose, tf->position, yaw,
                                     parts, entity::MAX_PARTS);
        for (u8 k = 0; k < n; ++k) {
            MobInstance inst{};
            inst.pos   = parts[k].center;
            inst.size  = parts[k].size;
            inst.colorGpu = packInstanceColor(parts[k].color);
            inst.rot   = glm::vec4(parts[k].rot.x, parts[k].rot.y,
                                   parts[k].rot.z, parts[k].rot.w);
            cpu_.push_back(inst);
        }
    }
}

void NpcRenderer::upload(vk::Context& ctx) {
    (void)ctx;
    instanceCount_ = (u32)cpu_.size();
    if (instanceCount_ == 0) return;
    // Пишем прямо в память, видимую процессору: ни временного буфера,
    // ни отдельной отправки в очередь, ни ожидания GPU.
    if (!instances_.write(cpu_.data(),
                          (u64)instanceCount_ * sizeof(MobInstance)))
        instanceCount_ = 0;
}

void NpcRenderer::render(vk::Context& ctx, VkDescriptorSet set) {
    if (instanceCount_ == 0 || !instances_.handle() || !pipeline_.valid()) return;
    VkCommandBuffer cmd = ctx.currentCmd();

    ctx.setFullViewport(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_.layout(), 0, 1, &set, 0, nullptr);
    VkBuffer vbs[2] = { vbo_.handle(), instances_.handle() };
    VkDeviceSize offs[2] = { 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
    vkCmdBindIndexBuffer(cmd, ibo_.handle(), 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, 36, instanceCount_, 0, 0, 0);
}

void NpcRenderer::destroy() {
    vbo_.destroy();
    ibo_.destroy();
    instances_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render

/**
 * @file npc_renderer.cpp
 * @brief Рендер: меширование чанков, LOD, отсечение, инстансинг, камера.
 */
#include "npc_renderer.h"
#include "../npc/npc_def.h"
#include "../npc/npc_ai.h"
#include "../ecs/components.h"
#include "../core/orientation.h"
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

} // namespace

bool NpcRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
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

    cpu_.reserve(128);
    LOGI("NpcRenderer готов");
    return true;
}

void NpcRenderer::rebuild(ecs::Registry& reg) {
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

        // Смерть
        f32 deathSink = 0.f;
        if (ai->state == npc::NpcAI::Dead) {
            deathSink = std::min(0.8f, ai->deathTimer * 0.5f);
        }

        // --- Тело ---
        {
            MobInstance inst{};
            inst.pos   = tf->position + glm::vec3(0, -deathSink, 0);
            inst.size  = glm::vec3(0.55f, 0.85f, 0.35f);
            inst.color = def.bodyColor;
            inst.yaw   = yaw;

            if (ai->damageFlash > 0.f) {
                f32 t = ai->damageFlash / 0.15f;
                u8 r = (inst.color >> 24) & 0xFF;
                u8 g = (inst.color >> 16) & 0xFF;
                u8 b = (inst.color >>  8) & 0xFF;
                r = (u8)(r * (1.f - t) + 255.f * t);
                g = (u8)(g * (1.f - t) +  40.f * t);
                b = (u8)(b * (1.f - t) +  40.f * t);
                inst.color = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFF;
            }
            cpu_.push_back(inst);
        }

        // --- Голова ---
        {
            MobInstance inst{};
            inst.pos   = tf->position + glm::vec3(0, 1.45f - deathSink, 0);
            inst.size  = glm::vec3(0.5f, 0.5f, 0.5f);
            inst.color = def.headColor;
            inst.yaw   = yaw;
            cpu_.push_back(inst);
        }

        // --- Ноги (4 коробки) ---
        const f32 legSwing = std::sin(ai->walkPhase) * 0.15f * speedNorm;
        const f32 legSwingOpp = -legSwing;
        const f32 legOffsetY = 0.35f - deathSink;

        const glm::vec3 legOffsets[4] = {
            {  0.18f, legOffsetY,  0.15f + legSwing    },
            { -0.18f, legOffsetY,  0.15f + legSwingOpp },
            {  0.18f, legOffsetY, -0.15f + legSwingOpp },
            { -0.18f, legOffsetY, -0.15f + legSwing    },
        };
        for (const auto& off : legOffsets) {
            MobInstance inst{};
            // Тот же поворот, что и в шейдере. Раньше здесь стоял
            // поворот противоположной ручности, и тело собиралось
            // наизнанку при любом движении с составляющей по X.
            inst.pos = tf->position + orient::rotateY(off, yaw);
            inst.size  = glm::vec3(0.18f, 0.7f, 0.18f);
            inst.color = def.accentColor;
            inst.yaw   = yaw;
            cpu_.push_back(inst);
        }

        // --- Иконка над головой (квестодатель) ---
        if (def.role == npc::NpcRole::QuestGiver &&
            ai->state != npc::NpcAI::Dead)
        {
            bool hasAvailable =
                (ai->offeredQuest != 0) || true; // всегда есть доступный квест
            MobInstance inst{};
            inst.pos   = tf->position + glm::vec3(0, 2.15f, 0);
            inst.size  = glm::vec3(0.35f, 0.35f, 0.1f);
            inst.color = hasAvailable
                ? 0xFFD040FF      // жёлтый "!"
                : 0xA0A0A0FF;     // серый
            inst.yaw   = yaw;
            cpu_.push_back(inst);
        }

        // --- Иконка торговца ---
        if (def.role == npc::NpcRole::Trader &&
            ai->state != npc::NpcAI::Dead)
        {
            MobInstance inst{};
            inst.pos   = tf->position + glm::vec3(0, 2.15f, 0);
            inst.size  = glm::vec3(0.30f, 0.30f, 0.1f);
            inst.color = 0xFFC040FF;   // золотая монета
            inst.yaw   = yaw;
            cpu_.push_back(inst);
        }
    }

    instanceCount_ = (u32)cpu_.size();
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

void NpcRenderer::destroy() {
    vbo_.destroy();
    ibo_.destroy();
    instances_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render

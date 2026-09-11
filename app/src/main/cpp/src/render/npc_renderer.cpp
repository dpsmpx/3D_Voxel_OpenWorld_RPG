#include "npc_renderer.h"
#include "../npc/npc_def.h"
#include "../npc/npc_ai.h"
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
     0, 1, 2,  0, 2, 3,
     4, 5, 6,  4, 6, 7,
     8, 9,10,  8,10,11,
    12,13,14, 12,14,15,
    16,17,18, 16,18,19,
    20,21,22, 20,22,23,
};

} // namespace

bool NpcRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
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
    d.vertName    = "shaders/mob.vert.spv";
    d.fragName    = "shaders/mob.frag.spv";
    d.depthFormat = ctx.depthFormat();
    d.cullMode    = VK_CULL_MODE_BACK_BIT;
    d.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
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
        f32 yaw = 0.f;
        if (glm::length(velXZ) > 0.1f) {
            yaw = std::atan2(velXZ.x, velXZ.y);
        }
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
            f32 c = std::cos(yaw), s = std::sin(yaw);
            inst.pos = tf->position + glm::vec3(
                off.x * c - off.z * s,
                off.y,
                off.x * s + off.z * c);
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

void NpcRenderer::render(vk::Context& ctx) {
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

void NpcRenderer::destroy() {
    vbo_.destroy();
    ibo_.destroy();
    instanceGpu_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render

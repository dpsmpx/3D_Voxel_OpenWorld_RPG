/**
 * @file projectile_renderer.cpp
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#include "projectile_renderer.h"
#include "../combat/projectile.h"
#include "../ecs/components.h"
#include "../core/orientation.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace render {

// Вершинный формат и геометрия куба — общие для всех рендеров
// MobInstance, см. MOB_ATTRS в mob_renderer.h.

void precipInstances(const world::Precipitation& p, f32 snowMix,
                     std::vector<MobInstance>& out)
{
    out.clear();
    const bool snow = snowMix > 0.5f;

    // Дождь — штрих, а не точка. Капля, нарисованная кубиком, читается
    // как грязь на экране; вытянутая вдоль падения — как дождь.
    const glm::vec3 rainSize{ 0.035f, 0.035f, 0.85f };
    const glm::vec3 snowSize{ 0.11f,  0.11f,  0.11f };

    // Дождь почти прозрачен: сплошная сетка непрозрачных полос
    // закрывает мир. Снег плотный и белый — его и должно быть видно.
    const u32 rainColor = 0xA5BEDC78u;   // rgba(165,190,220,120)
    const u32 snowColor = 0xFAFAFFE1u;   // rgba(250,250,255,225)

    out.reserve(p.liveCount());
    for (const world::Drop& d : p.drops()) {
        if (!d.alive) continue;
        MobInstance inst{};
        inst.pos = d.pos;
        if (snow) {
            inst.size = snowSize;
            inst.colorGpu = packInstanceColor(snowColor);
            inst.rot = orient::yawQuat(d.sway);
        } else {
            inst.size = rainSize;
            inst.colorGpu = packInstanceColor(rainColor);
            inst.rot = orient::dirQuat(d.vel);
        }
        out.push_back(inst);
    }
}

void particleInstances(const world::Particles& p,
                       std::vector<MobInstance>& out)
{
    out.clear();
    out.reserve(p.liveCount());

    for (const world::Particle& q : p.items()) {
        if (!q.alive) continue;

        // Гаснет ПОД КОНЕЦ, а не с первого кадра: осколок, тающий
        // всю свою жизнь, виден только как муть. Полную прозрачность
        // он набирает за последнюю треть срока.
        const f32 left = q.lifeTime > 0.f ? q.life / q.lifeTime : 0.f;
        const f32 k = std::min(1.f, std::max(0.f, left * 3.f));
        const u8 a = (u8)(255.f * k * (f32)(q.color & 0xFFu) / 255.f);

        // Гасим ровно на столько, на сколько шейдер осветлит: осколок
        // камня обязан быть цвета камня, а не цвета камня в полтора
        // раза ярче.
        auto dim = [](u32 c, u32 shift) {
            const f32 v = (f32)((c >> shift) & 0xFFu) / PROJECTILE_FRAG_GAIN;
            return (u32)std::min(255.f, std::max(0.f, v)) << shift;
        };
        const u32 rgba = dim(q.color, 24) | dim(q.color, 16) | dim(q.color, 8)
                       | (u32)a;

        MobInstance inst{};
        inst.pos  = q.pos;
        inst.size = glm::vec3(q.size);
        inst.colorGpu = packInstanceColor(rgba);
        // Кувыркается: куб, стоящий гранями по осям мира, читается
        // как забытый в воздухе блок, а не как обломок.
        inst.rot = orient::dirQuat({ std::cos(q.spin),
                                     std::sin(q.spin * 1.7f),
                                     std::sin(q.spin) });
        out.push_back(inst);
    }
}

bool ProjectileRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
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
    d.vertName    = "shaders/projectile.vert.spv";
    d.fragName    = "shaders/projectile.frag.spv";
    d.depthFormat = ctx.depthFormat();
    d.cullMode    = VK_CULL_MODE_NONE;
    d.depthTest   = true;
    d.depthWrite  = false;
    d.blend       = true;
    d.bindings     = MOB_BINDINGS;
    d.bindingCount = MOB_BINDING_COUNT;
    d.attrs        = MOB_ATTRS;
    d.attrCount    = MOB_ATTR_COUNT;
    if (!pipeline_.create(dev_, shaders_, d)) return false;

    cpu_.reserve(256);
    LOGI("ProjectileRenderer готов");
    return true;
}

void ProjectileRenderer::setPrecip(const MobInstance* data, u32 count) {
    precip_.assign(data, data + count);
    precipCount_ = count;
}

void ProjectileRenderer::setParticles(const MobInstance* data, u32 count) {
    particles_.assign(data, data + count);
    particleCount_ = count;
}

void ProjectileRenderer::rebuild(ecs::Registry& reg) {
    cpu_.clear();

    // --- Снаряды ---
    {
        auto& pool = reg.pool<combat::Projectile>();
        for (usize i = 0; i < pool.size(); ++i) {
            ecs::Entity e = pool.entityAt((u32)i);
            auto* p  = pool.get(e);
            auto* tf = reg.get<ecs::Transform>(e);
            if (!p || !tf) continue;

            MobInstance inst{};
            inst.pos   = tf->position;
            inst.colorGpu = packInstanceColor(p->colorRGBA);

            // Снаряд летит вдоль своей скорости — и выглядеть обязан
            // так же. Заклинание это сгусток, ему направление не
            // нужно; стрела — древко, и раньше она летела кубом,
            // потому что формат инстанса поворота не нёс вовсе.
            if (p->isSpell) {
                inst.size = glm::vec3(p->scale * 2.f);
                inst.rot  = orient::yawQuat(0.f);
            } else {
                inst.size = glm::vec3(p->scale, p->scale, p->scale * 6.f);
                inst.rot  = orient::dirQuat(p->velocity);
            }
            cpu_.push_back(inst);
        }
    }

    // --- HitFx ---
    {
        auto& pool = reg.pool<combat::HitFx>();
        for (usize i = 0; i < pool.size(); ++i) {
            ecs::Entity e = pool.entityAt((u32)i);
            auto* fx = pool.get(e);
            auto* tf = reg.get<ecs::Transform>(e);
            if (!fx || !tf) continue;

            f32 t = 1.f - fx->lifeRemaining / std::max(0.001f, fx->lifeTime);
            f32 s = fx->startScale + (fx->endScale - fx->startScale) * t;

            // Затухание альфы
            u8 a = (u8)(255.f * (1.f - t));

            u32 c = fx->colorRGBA;
            u8 r = (c >> 24) & 0xFF;
            u8 g = (c >> 16) & 0xFF;
            u8 b = (c >>  8) & 0xFF;
            u32 faded = ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | a;

            MobInstance inst{};
            inst.pos   = tf->position;
            inst.size  = glm::vec3(s);
            inst.colorGpu = packInstanceColor(faded);
            inst.rot = orient::yawQuat(0.f);
            cpu_.push_back(inst);
        }
    }

    // --- Осколки ---
    //
    // После снарядов и вспышек, но ДО осадков: щепка сквозь дождь —
    // это то, что нужно, дождь сквозь щепку — нет.
    if (!particles_.empty())
        cpu_.insert(cpu_.end(), particles_.begin(), particles_.end());

    // --- Осадки ---
    //
    // Последними: они полупрозрачные и без записи глубины, а значит
    // порядок между ними и снарядами виден. Снаряд сквозь дождь —
    // это то, что нужно; дождь сквозь снаряд — нет.
    if (!precip_.empty())
        cpu_.insert(cpu_.end(), precip_.begin(), precip_.end());

    instanceCount_ = (u32)cpu_.size();
}

void ProjectileRenderer::upload(vk::Context& ctx) {
    (void)ctx;
    instanceCount_ = (u32)cpu_.size();
    if (instanceCount_ == 0) return;
    // Пишем прямо в память, видимую процессору: ни временного буфера,
    // ни отдельной отправки в очередь, ни ожидания GPU.
    if (!instances_.write(cpu_.data(),
                          (u64)instanceCount_ * sizeof(MobInstance)))
        instanceCount_ = 0;
}

void ProjectileRenderer::render(vk::Context& ctx, VkDescriptorSet set) {
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

void ProjectileRenderer::destroy() {
    vbo_.destroy();
    ibo_.destroy();
    instances_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render

/**
 * @file item_renderer.cpp
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#include "item_renderer.h"
#include "../items/throwable.h"
#include "../items/item_pickup.h"
#include "../items/item_def.h"
#include "../items/item_models.h"
#include "../ecs/components.h"
#include "../core/orientation.h"
#include "../core/log.h"
#include <cstring>
#include <cmath>
#include <vector>

namespace render {

// Вершинный формат и геометрия куба — общие для всех рендеров
// MobInstance, см. MOB_ATTRS в mob_renderer.h.

ModelPose pickupPose(const items::ItemPickup& p, const glm::vec3& at, f32 modelHeight) {
    // Поворот по месту: соседние предметы лежат вразнобой, а один и
    // тот же — всегда одинаково, кадр за кадром.
    const f32 yaw = at.x * 1.7f + at.z * 2.3f;
    ModelPose pose;
    if (p.onGround) {
        pose.rot = orient::yawQuat(yaw);
        // На волосок над землёй: иначе нижняя грань спорит с верхом
        // блока за один и тот же пиксель.
        pose.pos = at + glm::vec3(0.f, 0.004f, 0.f);
        return pose;
    }
    // В полёте кувыркается — вокруг своей середины, а не вокруг
    // точки, на которой лежал бы.
    const f32 angle = p.lifeRemaining * 7.f;
    pose.rot = orient::qmul(orient::yawQuat(yaw),
                            orient::axisQuat(glm::vec3(1.f, 0.35f, 0.6f), angle));
    const glm::vec3 mid{ 0.f, modelHeight * 0.5f, 0.f };
    pose.pos = at + mid - orient::qrot(pose.rot, mid);
    return pose;
}

u32 pickupCopies(u16 count) {
    return count >= 16 ? 3u : (count >= 2 ? 2u : 1u);
}

bool ItemRenderer::init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout) {
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
    // Выпавшие предметы — обычные кубы, а не свечение: им нужно то же
    // освещение, что мобам, иначе они горят ровным цветом даже ночью.
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

    cpu_.reserve(256);

    // Модели всех предметов — один раз, одним буфером. Номер модели —
    // номер предмета; у пустых номеров меш пустой, и рисовать их нечем.
    if (!models_.init(ctx, mgr, descLayout)) return false;
    std::vector<VoxelMesh> meshes(items::ITEM_COUNT);
    modelHeight_.assign(items::ITEM_COUNT, 0.f);
    for (u16 id = 1; id < items::ITEM_COUNT; ++id) {
        if (!items::items().get(id).name) continue;
        meshes[id] = meshVoxelModel(items::itemModel(id));
        modelHeight_[id] = meshes[id].boundsMax.y - meshes[id].boundsMin.y;
    }
    if (!models_.setModels(ctx, meshes)) return false;

    LOGI("ItemRenderer готов");
    return true;
}

void ItemRenderer::rebuild(ecs::Registry& reg) {
    cpu_.clear();
    models_.begin();

    auto& pool = reg.pool<items::ItemPickup>();
    for (usize i = 0; i < pool.size(); ++i) {
        ecs::Entity e = pool.entityAt((u32)i);
        auto* p  = pool.get(e);
        auto* tf = reg.get<ecs::Transform>(e);
        if (!p || !tf) continue;
        if (p->stack.empty()) continue;
        const u16 id = p->stack.itemId;
        if (id >= modelHeight_.size()) continue;

        // Перед тем как истлеть, предмет мигает. Модели рисуются без
        // смешивания, так что мигание — это кадры, в которые его нет.
        if (p->lifeRemaining < 5.f && std::sin(p->blinkTimer * 3.14159f) < 0.f) continue;

        const ModelPose pose = pickupPose(*p, tf->position, modelHeight_[id]);
        models_.add(id, pose.pos, pose.rot, 1.f, 0xFFFFFFFFu);

        // Стопка — горкой: ещё одна-две модели рядом, со своим
        // поворотом, чуть выше.
        const u32 copies = pickupCopies(p->stack.count);
        for (u32 k = 1; k < copies; ++k) {
            const glm::vec4 turn = orient::yawQuat(0.9f * (f32)k);
            const glm::vec4 rot = orient::qmul(pose.rot, turn);
            const glm::vec3 off = orient::qrot(pose.rot,
                glm::vec3(0.07f * (f32)k, 0.012f * (f32)k, -0.05f * (f32)k));
            models_.add(id, pose.pos + off, rot, 1.f, 0xFFFFFFFFu);
        }
    }

    // ---- Брошенные батуты ----
    //
    // Рисуются здесь же: это такая же лежащая в мире коробка, и
    // заводить ради неё отдельный конвейер не за чем. Площадка
    // низкая и широкая — по ней и видно, что это не предмет, который
    // можно подобрать.
    {
        auto& tramps = reg.pool<items::Trampoline>();
        for (usize i = 0; i < tramps.size(); ++i) {
            const ecs::Entity e = tramps.entityAt((u32)i);
            auto* t  = tramps.get(e);
            auto* tf = reg.get<ecs::Transform>(e);
            if (!t || !tf) continue;

            // Мигает перед тем, как исчезнуть, — как и подбираемое.
            u8 alpha = 255;
            if (t->lifeRemaining < 5.f &&
                std::sin(t->lifeRemaining * 8.f) < 0.f) alpha = 80;

            MobInstance inst{};
            inst.pos  = tf->position +
                        glm::vec3(0.f, items::TRAMPOLINE_PAD_H * 0.5f, 0.f);
            inst.size = glm::vec3(t->radius * 2.f,
                                  items::TRAMPOLINE_PAD_H,
                                  t->radius * 2.f);
            inst.colorGpu = packInstanceColor(0x2E6ED800u | alpha);
            inst.rot = orient::yawQuat(0.f);
            cpu_.push_back(inst);
        }
    }

    instanceCount_ = (u32)cpu_.size();
}

void ItemRenderer::upload(vk::Context& ctx) {
    models_.upload(ctx);
    instanceCount_ = (u32)cpu_.size();
    if (instanceCount_ == 0) return;
    // Пишем прямо в память, видимую процессору: ни временного буфера,
    // ни отдельной отправки в очередь, ни ожидания GPU.
    if (!instances_.write(cpu_.data(),
                          (u64)instanceCount_ * sizeof(MobInstance)))
        instanceCount_ = 0;
}

void ItemRenderer::render(vk::Context& ctx, VkDescriptorSet set) {
    models_.render(ctx, set);
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

void ItemRenderer::destroy() {
    models_.destroy();
    vbo_.destroy();
    ibo_.destroy();
    instances_.destroy();
    pipeline_.destroy();
    shaders_.destroyAll();
    dev_ = VK_NULL_HANDLE;
}

} // namespace render

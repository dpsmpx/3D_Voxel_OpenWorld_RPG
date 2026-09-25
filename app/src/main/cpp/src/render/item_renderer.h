/**
 * @file item_renderer.h
 * @brief Рендер: меширование чанков, отсечение, инстансинг, камера.
 */
#pragma once
#include "../core/types.h"
#include "../vk/vk_buffer.h"
#include "instance_ring.h"
#include "../vk/vk_pipeline.h"
#include "../vk/vk_shader.h"
#include "../vk/vk_context.h"
#include "../ecs/registry.h"
#include "mob_renderer.h"
#include "voxel_model_renderer.h"
#include "../items/item_pickup.h"
#include <android/asset_manager.h>
#include <glm/glm.hpp>
#include <vector>

namespace render {

/// Как лежит выпавший предмет: где начало его модели и как она
/// повёрнута. На земле — плашмя, со своим поворотом по месту; в
/// полёте — кувыркается вокруг своей середины.
struct ModelPose {
    glm::vec3 pos{0.f};
    glm::vec4 rot{0.f, 0.f, 0.f, 1.f};
};
ModelPose pickupPose(const items::ItemPickup& p, const glm::vec3& at, f32 modelHeight);

/// Сколько моделей рисовать на стопку: одна, две или три — горка
/// видна издалека, число всё равно покажет сумка.
u32 pickupCopies(u16 count);

class ItemRenderer {
public:
    bool init(vk::Context& ctx, AAssetManager* mgr, VkDescriptorSetLayout descLayout);
    void destroy();

    void rebuild(ecs::Registry& reg);
    void upload(vk::Context& ctx);
    /// set передаётся явно и привязывается своим layout'ом. Раньше
    /// дескрипторы брались те, что оставил после себя рендер чанков:
    /// Vulkan гарантирует их сохранность только при совместимых
    /// layout'ах, а совместимость ломается от любого расхождения —
    /// например от push-константы, которой у чанков теперь есть.
    void render(vk::Context& ctx, VkDescriptorSet set);

    u32 instanceCount() const { return instanceCount_ + models_.instanceCount(); }
    u32 drawCount() const { return (instanceCount_ ? 1u : 0u) + models_.drawCount(); }

private:
    VkDevice             dev_ = VK_NULL_HANDLE;
    vk::ShaderCache      shaders_;
    vk::GraphicsPipeline pipeline_;
    vk::Buffer           vbo_;
    vk::Buffer           ibo_;
    InstanceRing         instances_;

    /// Выпавшие предметы: у каждого своя воксельная модель, номер
    /// модели — номер предмета.
    VoxelModelRenderer models_;
    std::vector<f32>   modelHeight_;

    /// Батуты — это площадка, а не предмет: коробкой.
    std::vector<MobInstance> cpu_;
    u32 instanceCount_ = 0;
};

} // namespace render
